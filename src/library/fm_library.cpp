#include "library/fm_library.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <unordered_map>

#include <sqlite3.h>

namespace ion {
namespace fs = std::filesystem;

namespace {

// ---- SQLite helpers --------------------------------------------------------

class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
  ~Stmt() { sqlite3_finalize(s_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  bool ok() const { return s_ != nullptr; }
  Stmt& bind(int i, int64_t v) { sqlite3_bind_int64(s_, i, v); return *this; }
  Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); return *this; }
  Stmt& bind(int i, const uint8_t* data, std::size_t n) { sqlite3_bind_blob(s_, i, data, static_cast<int>(n), SQLITE_TRANSIENT); return *this; }
  bool step() { return sqlite3_step(s_) == SQLITE_ROW; }
  bool run() {
    const int rc = sqlite3_step(s_);
    sqlite3_reset(s_);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
  }
  void reset() { sqlite3_reset(s_); sqlite3_clear_bindings(s_); }
  int64_t i64(int c) const { return sqlite3_column_int64(s_, c); }
  std::string text(int c) const {
    const auto* t = sqlite3_column_text(s_, c);
    return t ? reinterpret_cast<const char*>(t) : "";
  }
  std::span<const uint8_t> blob(int c) const {
    const auto* b = static_cast<const uint8_t*>(sqlite3_column_blob(s_, c));
    return {b, b ? static_cast<std::size_t>(sqlite3_column_bytes(s_, c)) : 0u};
  }

 private:
  sqlite3_stmt* s_ = nullptr;
};

bool exec(sqlite3* db, const char* sql, std::string& error) {
  char* msg = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) == SQLITE_OK) return true;
  error = msg ? msg : "sqlite error";
  sqlite3_free(msg);
  return false;
}

constexpr int kSchemaVersion = 1;

const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS voice (
  id INTEGER PRIMARY KEY,
  hash INTEGER NOT NULL UNIQUE,     -- fmVoiceHash: every parameter except the name
  name TEXT NOT NULL,
  data BLOB NOT NULL,               -- 155-byte unpacked voice, normalised
  algorithm INTEGER NOT NULL,
  category TEXT NOT NULL,
  favourite INTEGER NOT NULL DEFAULT 0,
  last_used INTEGER NOT NULL DEFAULT 0,
  added INTEGER NOT NULL,
  origin TEXT NOT NULL DEFAULT 'import'  -- 'import' or 'user' (save as)
);
CREATE TABLE IF NOT EXISTS source (
  voice_id INTEGER NOT NULL REFERENCES voice(id) ON DELETE CASCADE,
  bank TEXT NOT NULL,               -- file name without extension
  path TEXT NOT NULL,
  slot INTEGER NOT NULL,            -- voice number in the file, from 0
  UNIQUE (voice_id, path, slot)
);
CREATE INDEX IF NOT EXISTS source_voice ON source(voice_id);
CREATE TABLE IF NOT EXISTS tag (
  voice_id INTEGER NOT NULL REFERENCES voice(id) ON DELETE CASCADE,
  tag TEXT NOT NULL,
  UNIQUE (voice_id, tag)
);
CREATE TABLE IF NOT EXISTS import_run (
  id INTEGER PRIMARY KEY,
  time INTEGER NOT NULL,
  folder TEXT NOT NULL,
  files INTEGER NOT NULL, banks INTEGER NOT NULL, voices INTEGER NOT NULL,
  added INTEGER NOT NULL, corrupt INTEGER NOT NULL
);
)";

sqlite3* openDb(const fs::path& path, std::string& error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    error = db ? sqlite3_errmsg(db) : "can't open " + path.string();
    sqlite3_close(db);
    return nullptr;
  }
  sqlite3_busy_timeout(db, 5000);  // an import on another connection holds the write lock for a while
  int version = 0;
  {
    Stmt v(db, "PRAGMA user_version");
    if (v.step()) version = static_cast<int>(v.i64(0));
  }
  if (version > kSchemaVersion) {
    error = "library.db was made by a newer ion (schema " + std::to_string(version) + ")";
    sqlite3_close(db);
    return nullptr;
  }
  if (!exec(db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;", error) || !exec(db, kSchema, error) ||
      !exec(db, ("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str(), error)) {
    sqlite3_close(db);
    return nullptr;
  }
  return db;
}

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool importable(const fs::path& p) {
  const std::string ext = lower(p.extension().string());
  return ext == ".syx" || ext == ".snd";
}

// A headerless 4096-byte file is only trusted if it looks like voice data:
// most of its names are printable text.
bool plausibleNames(const std::vector<syx::Voice>& voices, const std::vector<uint8_t>& packed) {
  int good = 0;
  for (std::size_t v = 0; v < 32; ++v) {
    bool printable = true;
    for (std::size_t i = 0; i < syx::kNameLength; ++i) {
      const uint8_t c = packed[v * syx::kPackedVoiceSize + 118 + i];
      if (c < 32 || c > 126) printable = false;
    }
    good += printable;
  }
  return good >= 24 && voices.size() == 32;
}

constexpr std::uintmax_t kMaxFileBytes = 16u << 20;  // big multi-bank archives are a few hundred KB

}  // namespace

// ---- categories ---------------------------------------------------------------

namespace {

constexpr const char* kCategories[] = {"bass", "keys", "organ", "brass", "strings", "pad", "lead", "bell",
                                       "pluck", "wind", "voice", "synth", "perc", "fx", "other"};

struct Rule {
  const char* category;
  std::vector<const char*> words;  // substrings of the upper-cased name
};

// First match wins, so the order matters: "SYN BASS" is bass, "BRASS PAD" is
// brass, "E.PIANO" is keys. These are deliberately rough (HANDOFF §6.3);
// tags are how a patch really gets found.
const std::vector<Rule>& rules() {
  static const std::vector<Rule> r = {
      {"perc", {"DRUM", "SNARE", "KICK", " TOM ", "TOMS", "HIHAT", "HI-HAT", " HAT ", "CYMB", "CONGA", "BONGO",
                "TIMPANI", "TYMPANI", "PERC", "COWBELL", "CLAP", "WOODBLO", "TABLA", "SHAKER", "STEEL DR", "STEELDR",
                "BD ", "SNR", "SNAR", "TAMBOUR", "CASTAN", "CASTON", "TIMBAL", "TAIKO", "CRASH", "CYM", " RIDE ",
                "BLOCK", "ANVIL", "AGOGO"}},
      {"bass", {"BASS", " BAS ", " BAS-", "-BAS", " BS ", " SUB ", "FRETLESS", "SLAP"}},
      {"keys", {"PIANO", "PIANA", "E.P", "EPIAN", "E-PIA", " EP ", " EP-", " EP.", "RHOD", "WURLI", "CLAV", "CLV", "HARPSI",
                "CEMBALO", "KEYS", "DYNO", "CP70", "CP-70", "CP80", "TINE", "PNO", "PIA", "GRAND", "STEINWAY",
                "UPRIGHT", "BOSDOR"}},
      {"wind", {"PANPIPE", "PAN PIPE"}},
      {"organ", {"ORGAN", "ORGN", "ORG", "HAMMOND", " B3 ", "B-3", "PIPE ORG", "PIPES", "CHURCH",
                 "KIRCHE", "CATHEDRAL", "LESLIE", "DRAWB", "FARFISA", "HARMONIUM", "ACCORD", "CALIOPE"}},
      {"brass", {"BRASS", "BRAS", "BRS", "TRUMP", "TRPT", "TROMB", "TROMP", "HORN", "HRN", "TUBA", "FANFAR",
                 "FLUEGEL", "FLUGEL"}},
      {"strings", {"STRING", "STRNG", " STR ", " STR.", "STRG", "STNG", "STG", "VIOL", "CELLO", "CLLO", "FIDDLE", " BOW", "ORCH", "ENSEMB",
                   "PIZZ"}},
      {"voice", {"CHOIR", "CHORAL", "VOICE", "VOIC", "VOX", "VOCAL", "VCE", "VOWEL", "AAH", "OOH", "HUMAN", "SCAT"}},
      {"pad", {"PAD", "SWELL", "WARM", "SOFT", "ATMOS", "STRATO", "SPACE", "DREAM"}},
      {"bell", {"BELL", "CHIME", "GLOCK", "VIBE", "VIBRA", "MARIMB", "XYLO", "CELEST", "MALLET", "KALIMB",
                "RIMBA", "MRMBA", "TUB", "GAMELAN", "GLASS", "MUSIC BOX"}},
      {"pluck", {"GUITAR", "GUIT", "GTR", "HARP", "PLUCK", "KOTO", "SITAR", "BANJO", "MANDOL", " LUTE", "SHAMI",
                 "DULCI", "ZITHER", "NYLON", "STEEL", "BIWA"}},
      {"wind", {"FLUTE", "FLUT", "SAX", "CLARI", "OBOE", "BASSOON", "PICCOL", "RECORDER",
                "SHAKU", "OCARIN", "WHISTL", "HARMONICA", "REED", "FLOOT", "FLOETE", "AULOS"}},
      {"lead", {"LEAD", "SOLO", " LD "}},
      {"synth", {"SYN", "SAW", "SQUARE", "SQR", "ANALOG", "ANLG", "SYNC", "OBX", "OB-", "PROPHET", "JUNO",
                 "MOOG", "ARP", "POLY", "PWM"}},
      {"fx", {"FX", "EFFECT", "NOISE", " WIND ", "THUNDER", "RAIN", "TRAIN", "TAKE OFF", "LASER", "ZAP", "SWEEP",
              "SIREN", "EXPLO", "BIRD", "WAVE", "UFO", "ALIEN", "ROBOT", "WHOOSH", "SCRATCH",
              "TELEPHONE", "STORM", "IMPACT", "INDUST"}},
  };
  return r;
}

}  // namespace

std::span<const char* const> fmCategories() { return kCategories; }

std::string fmCategory(const std::string& name) {
  // Padded with spaces, so " EP " matches a whole word anywhere, including
  // at either end, and never the end of "DEEP".
  const std::string n = " " + upper(name) + " ";
  for (const Rule& rule : rules())
    for (const char* w : rule.words)
      if (n.find(w) != std::string::npos) return rule.category;
  return "other";
}

uint64_t fmVoiceHash(const syx::Voice& voice) {
  // FNV-1a, 64-bit, over bytes 0-144. With tens of thousands of voices the
  // chance of any collision is around 1 in 10^10.
  uint64_t h = 14695981039346656037ull;
  for (std::size_t i = 0; i < syx::kNameOffset; ++i) {
    h ^= voice[i];
    h *= 1099511628211ull;
  }
  return h;
}

// ---- FmLibrary ------------------------------------------------------------------

FmLibrary::~FmLibrary() { sqlite3_close(db_); }

std::unique_ptr<FmLibrary> FmLibrary::open(const fs::path& dbPath, std::string& error) {
  sqlite3* db = openDb(dbPath, error);
  if (!db) return nullptr;
  std::unique_ptr<FmLibrary> lib(new FmLibrary());
  lib->db_ = db;
  if (!lib->reload(error)) return nullptr;
  return lib;
}

bool FmLibrary::reload(std::string& error) {
  std::vector<FmPatch> out;
  std::unordered_map<int64_t, std::size_t> byId;
  {
    Stmt q(db_,
           "SELECT v.id, v.name, v.data, v.algorithm, v.category, v.favourite, v.last_used, v.origin, "
           "(SELECT bank FROM source s WHERE s.voice_id = v.id ORDER BY s.rowid LIMIT 1), "
           "(SELECT COUNT(*) FROM source s WHERE s.voice_id = v.id) FROM voice v");
    if (!q.ok()) {
      error = sqlite3_errmsg(db_);
      return false;
    }
    while (q.step()) {
      FmPatch p;
      p.id = q.i64(0);
      p.name = q.text(1);
      const auto blob = q.blob(2);
      if (blob.size() != syx::kVoiceSize) continue;
      std::copy(blob.begin(), blob.end(), p.voice.begin());
      p.algorithm = static_cast<int>(q.i64(3));
      p.category = q.text(4);
      p.favourite = q.i64(5) != 0;
      p.lastUsed = q.i64(6);
      p.user = q.text(7) == "user";
      p.source = p.user ? "saved" : q.text(8);
      p.sourceCount = static_cast<int>(q.i64(9));
      byId[p.id] = out.size();
      out.push_back(std::move(p));
    }
  }
  {
    Stmt q(db_, "SELECT voice_id, tag FROM tag ORDER BY tag");
    while (q.step())
      if (auto it = byId.find(q.i64(0)); it != byId.end()) out[it->second].tags.push_back(q.text(1));
  }
  patches_ = std::move(out);
  sortPatches();
  return true;
}

void FmLibrary::sortPatches() {
  std::sort(patches_.begin(), patches_.end(), [](const FmPatch& a, const FmPatch& b) {
    const std::string x = upper(a.name), y = upper(b.name);
    return x != y ? x < y : a.id < b.id;
  });
}

const FmPatch* FmLibrary::find(int64_t id) const {
  for (const FmPatch& p : patches_)
    if (p.id == id) return &p;
  return nullptr;
}

const FmPatch* FmLibrary::findVoice(const syx::Voice& voice) const {
  const uint64_t h = fmVoiceHash(syx::normalize(voice));
  for (const FmPatch& p : patches_)
    if (fmVoiceHash(p.voice) == h) return &p;
  return nullptr;
}

std::vector<int> FmLibrary::filter(const std::string& text, const std::string& category, bool favouritesOnly,
                                   bool recentOnly) const {
  std::vector<std::string> words;
  {
    std::string w;
    for (char c : lower(text) + " ") {
      if (c == ' ' || c == ',') {
        if (!w.empty()) words.push_back(w);
        w.clear();
      } else {
        w += c;
      }
    }
  }
  std::vector<int> out;
  for (int i = 0; i < static_cast<int>(patches_.size()); ++i) {
    const FmPatch& p = patches_[static_cast<std::size_t>(i)];
    if (favouritesOnly && !p.favourite) continue;
    if (recentOnly && p.lastUsed == 0) continue;
    if (!category.empty() && p.category != category) continue;
    const std::string name = lower(p.name), source = lower(p.source);
    bool all = true;
    for (const std::string& w : words) {
      bool hit = name.find(w) != std::string::npos || p.category == w || source.find(w) != std::string::npos;
      for (const std::string& t : p.tags) hit = hit || t.rfind(w, 0) == 0;
      if (!hit) {
        all = false;
        break;
      }
    }
    if (all) out.push_back(i);
  }
  if (recentOnly)
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
      return patches_[static_cast<std::size_t>(a)].lastUsed > patches_[static_cast<std::size_t>(b)].lastUsed;
    });
  return out;
}

bool FmLibrary::setFavourite(int64_t id, bool on) {
  Stmt q(db_, "UPDATE voice SET favourite = ? WHERE id = ?");
  if (!q.bind(1, on ? 1 : 0).bind(2, id).run()) return false;
  for (FmPatch& p : patches_)
    if (p.id == id) p.favourite = on;
  return true;
}

bool FmLibrary::setTags(int64_t id, const std::vector<std::string>& tags) {
  std::string error;
  if (!exec(db_, "BEGIN", error)) return false;
  Stmt del(db_, "DELETE FROM tag WHERE voice_id = ?");
  del.bind(1, id).run();
  Stmt ins(db_, "INSERT OR IGNORE INTO tag (voice_id, tag) VALUES (?, ?)");
  std::vector<std::string> clean;
  for (const std::string& t : tags) {
    const std::string l = lower(t);
    if (l.empty() || std::find(clean.begin(), clean.end(), l) != clean.end()) continue;
    clean.push_back(l);
    ins.reset();
    ins.bind(1, id).bind(2, l).run();
  }
  if (!exec(db_, "COMMIT", error)) return false;
  std::sort(clean.begin(), clean.end());
  for (FmPatch& p : patches_)
    if (p.id == id) p.tags = clean;
  return true;
}

bool FmLibrary::markUsed(int64_t id) {
  const int64_t t = now();
  Stmt q(db_, "UPDATE voice SET last_used = ? WHERE id = ?");
  if (!q.bind(1, t).bind(2, id).run()) return false;
  for (FmPatch& p : patches_)
    if (p.id == id) p.lastUsed = t;
  return true;
}

std::vector<std::string> FmLibrary::sources(int64_t id) const {
  std::vector<std::string> out;
  Stmt q(db_, "SELECT DISTINCT bank FROM source WHERE voice_id = ? ORDER BY rowid");
  q.bind(1, id);
  while (q.step()) out.push_back(q.text(0));
  return out;
}

int64_t FmLibrary::addUser(const syx::Voice& voice, const std::vector<std::string>& tags, std::string& error) {
  const syx::Voice v = syx::normalize(voice);
  if (const FmPatch* existing = findVoice(v)) return existing->id;
  const std::string name = syx::name(v);
  Stmt q(db_,
         "INSERT INTO voice (hash, name, data, algorithm, category, added, origin) VALUES (?, ?, ?, ?, ?, ?, 'user')");
  q.bind(1, static_cast<int64_t>(fmVoiceHash(v))).bind(2, name).bind(3, v.data(), v.size());
  q.bind(4, v[134]).bind(5, fmCategory(name)).bind(6, now());
  if (!q.run()) {
    error = sqlite3_errmsg(db_);
    return 0;
  }
  const int64_t id = sqlite3_last_insert_rowid(db_);
  if (!tags.empty()) setTags(id, tags);
  if (!reload(error)) return 0;
  return id;
}

std::optional<FmLibrary::LastImport> FmLibrary::lastImport() const {
  Stmt q(db_, "SELECT time, folder, banks, voices, added, corrupt FROM import_run ORDER BY id DESC LIMIT 1");
  if (!q.step()) return std::nullopt;
  return LastImport{q.i64(0), q.text(1), static_cast<int>(q.i64(2)), static_cast<int>(q.i64(3)),
                    static_cast<int>(q.i64(4)), static_cast<int>(q.i64(5))};
}

// ---- import ------------------------------------------------------------------------

ImportReport FmLibrary::import(const fs::path& dbPath, const fs::path& folder, const fs::path& logPath,
                               ImportProgress* progress) {
  ImportReport report;
  sqlite3* db = openDb(dbPath, report.error);
  if (!db) return report;

  // Files first, in a stable order, so "the first-seen name" means the same
  // thing every time the same folder is imported.
  std::vector<fs::path> files;
  std::error_code ec;
  if (fs::is_regular_file(folder, ec)) {
    files.push_back(folder);
  } else if (fs::is_directory(folder, ec)) {
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec) && importable(it->path())) files.push_back(it->path());
    }
  } else {
    report.error = "no such file or folder: " + folder.string();
    sqlite3_close(db);
    return report;
  }
  std::sort(files.begin(), files.end());
  if (progress) progress->filesTotal = static_cast<int>(files.size());

  std::string error;
  exec(db, "BEGIN IMMEDIATE", error);
  {
    Stmt findHash(db, "SELECT id FROM voice WHERE hash = ?");
    Stmt insVoice(db, "INSERT INTO voice (hash, name, data, algorithm, category, added) VALUES (?, ?, ?, ?, ?, ?)");
    Stmt insSource(db, "INSERT OR IGNORE INTO source (voice_id, bank, path, slot) VALUES (?, ?, ?, ?)");
    const int64_t t = now();

    for (const fs::path& file : files) {
      if (progress && progress->cancel) break;
      ++report.files;
      const std::string shown = fs::relative(file, fs::is_directory(folder) ? folder : file.parent_path(), ec).string();
      std::vector<uint8_t> bytes;
      const auto size = fs::file_size(file, ec);
      if (!ec && size <= kMaxFileBytes) {
        std::ifstream in(file, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
      if (bytes.empty()) {
        ++report.unreadable;
        report.problems.push_back(shown + ": " + (ec ? "can't read" : size > kMaxFileBytes ? "too big" : "empty"));
        if (progress) ++progress->filesDone;
        continue;
      }
      const syx::ParseResult parsed = syx::parse(bytes);
      if (parsed.dumps.empty()) {
        ++report.unreadable;
        report.problems.push_back(shown + ": no DX7 voice data");
      }
      int slot = 0;
      const std::string bank = file.stem().string();
      for (const syx::Dump& dump : parsed.dumps) {
        const std::vector<syx::Voice> voices = syx::voices(dump);
        const int first = slot;
        slot += static_cast<int>(voices.size());
        // Log and skip corrupt banks (HANDOFF §6.3). Headerless dumps have no
        // checksum, so they have to look like voice data instead.
        if (!dump.checksum_ok) {
          ++report.corrupt;
          report.problems.push_back(shown + ": checksum mismatch at byte " + std::to_string(dump.offset) + ", skipped");
          continue;
        }
        if (dump.kind == syx::DumpKind::Headerless && !plausibleNames(voices, dump.data)) {
          ++report.corrupt;
          report.problems.push_back(shown + ": 4096 bytes but not DX7 voice names, skipped");
          continue;
        }
        ++report.banks;
        for (std::size_t i = 0; i < voices.size(); ++i) {
          const syx::Voice& v = voices[i];
          ++report.voices;
          const auto hash = static_cast<int64_t>(fmVoiceHash(v));
          int64_t id = 0;
          findHash.reset();
          findHash.bind(1, hash);
          if (findHash.step()) {
            id = findHash.i64(0);
          } else {
            const std::string name = syx::name(v);
            insVoice.reset();
            insVoice.bind(1, hash).bind(2, name).bind(3, v.data(), v.size()).bind(4, v[134]);
            insVoice.bind(5, fmCategory(name)).bind(6, t);
            if (!insVoice.run()) continue;
            id = sqlite3_last_insert_rowid(db);
            ++report.added;
          }
          insSource.reset();
          insSource.bind(1, id).bind(2, bank).bind(3, file.string()).bind(4, first + static_cast<int>(i)).run();
        }
      }
      if (progress) ++progress->filesDone;
    }
    Stmt run(db, "INSERT INTO import_run (time, folder, files, banks, voices, added, corrupt) VALUES (?, ?, ?, ?, ?, ?, ?)");
    run.bind(1, t).bind(2, folder.string()).bind(3, report.files).bind(4, report.banks).bind(5, report.voices);
    run.bind(6, report.added).bind(7, report.corrupt).run();
  }
  if (!exec(db, "COMMIT", error)) report.error = error;
  sqlite3_close(db);

  // import.log: appended, newest at the bottom.
  fs::create_directories(logPath.parent_path(), ec);
  if (std::ofstream log(logPath, std::ios::app); log) {
    char when[32];
    const std::time_t tt = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
    log << "== " << when << " import " << folder.string() << "\n";
    log << report.files << " files, " << report.banks << " banks, " << report.voices << " voices, " << report.added
        << " new, " << report.corrupt << " corrupt dumps skipped, " << report.unreadable << " files without voice data\n";
    for (const std::string& p : report.problems) log << "  " << p << "\n";
    if (!report.error.empty()) log << "  error: " << report.error << "\n";
  }
  return report;
}

}  // namespace ion
