// The FM library: import, de-duplication, browsing data (HANDOFF §6.3).

#include <doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include "library/fm_library.h"
#include "library/syx.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

// A random, valid voice with a given name. Generated, so the tests need no
// copyrighted bank.
syx::Voice voice(uint32_t seed, const char* name) {
  std::mt19937 rng(seed);
  syx::Voice v{};
  for (std::size_t i = 0; i < syx::kNameOffset; ++i) v[i] = static_cast<uint8_t>(rng() % 100);
  v = syx::normalize(v);
  std::string padded = name;
  padded.resize(syx::kNameLength, ' ');  // a DX7 name is exactly 10 characters, space-padded
  std::memcpy(v.data() + syx::kNameOffset, padded.data(), syx::kNameLength);
  return v;
}

syx::Voice renamed(syx::Voice v, const char* name) {
  std::string padded = name;
  padded.resize(syx::kNameLength, ' ');  // a DX7 name is exactly 10 characters, space-padded
  std::memcpy(v.data() + syx::kNameOffset, padded.data(), syx::kNameLength);
  return v;
}

std::vector<uint8_t> bulk(const std::vector<syx::Voice>& voices) { return syx::makeBanks(voices).at(0); }

void write(const fs::path& p, const std::vector<uint8_t>& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct Fixture {
  fs::path root = fs::temp_directory_path() / ("ion-library-" + std::to_string(std::random_device{}()));
  fs::path db = root / "library.db";
  fs::path log = root / "import.log";
  fs::path syx = root / "syx";
  std::vector<syx::Voice> a, b;

  Fixture() {
    // bank_a: 32 distinct voices.
    for (uint32_t i = 0; i < 32; ++i) a.push_back(voice(i + 1, ("A" + std::to_string(i)).c_str()));
    // bank_b: the first 16 of bank_a under other names, then 16 new ones.
    for (uint32_t i = 0; i < 16; ++i) b.push_back(renamed(a[i], ("B" + std::to_string(i)).c_str()));
    for (uint32_t i = 16; i < 32; ++i) b.push_back(voice(i + 100, ("B" + std::to_string(i)).c_str()));
    write(syx / "bank_a.syx", bulk(a));
    write(syx / "sub" / "bank_b.SYX", bulk(b));
  }
  ~Fixture() { fs::remove_all(root); }
};

}  // namespace

TEST_CASE("library: import de-duplicates by parameters, keeps the first name, records every source") {
  Fixture f;
  const ImportReport r = FmLibrary::import(f.db, f.syx, f.log);
  CHECK(r.error.empty());
  CHECK(r.files == 2);
  CHECK(r.banks == 2);
  CHECK(r.voices == 64);
  CHECK(r.added == 48);
  CHECK(r.corrupt == 0);

  std::string error;
  auto lib = FmLibrary::open(f.db, error);
  REQUIRE(lib);
  CHECK(lib->patches().size() == 48);
  const FmPatch* shared = lib->findVoice(f.a[3]);
  REQUIRE(shared);
  CHECK(shared->name == "A3");  // bank_a sorts first, so its names win
  CHECK(shared->source == "bank_a");
  CHECK(shared->sourceCount == 2);
  CHECK(lib->sources(shared->id) == std::vector<std::string>{"bank_a", "bank_b"});
  CHECK(lib->findVoice(f.b[20])->sourceCount == 1);
  CHECK(lib->findVoice(renamed(f.a[3], "WHATEVER"))->id == shared->id);  // names don't matter

  // Importing the same folder again adds nothing.
  const ImportReport again = FmLibrary::import(f.db, f.syx, f.log);
  CHECK(again.added == 0);
  REQUIRE(lib->reload(error));
  CHECK(lib->patches().size() == 48);
  CHECK(lib->findVoice(f.a[3])->sourceCount == 2);
  const auto last = lib->lastImport();
  REQUIRE(last);
  CHECK(last->voices == 64);
  CHECK(last->added == 0);
}

TEST_CASE("library: every dump format, and corrupt banks are logged and skipped") {
  Fixture f;
  std::vector<syx::Voice> c;
  for (uint32_t i = 0; i < 32; ++i) c.push_back(voice(i + 500, ("C" + std::to_string(i)).c_str()));

  // Two bulk dumps concatenated, with another sysex message in between.
  std::vector<uint8_t> two = bulk(c);
  const std::vector<uint8_t> other = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
  two.insert(two.end(), other.begin(), other.end());
  const auto d = bulk({voice(900, "D0")});
  two.insert(two.end(), d.begin(), d.end());
  write(f.syx / "two.syx", two);
  // A single-voice dump.
  write(f.syx / "single.syx", syx::makeSingleDump(voice(901, "SINGLE")));
  // A bad checksum.
  auto bad = bulk({voice(902, "BAD")});
  bad[100] ^= 0x01;
  write(f.syx / "bad.syx", bad);
  // A headerless 4096-byte bank, and 4096 bytes of noise.
  const auto e = bulk({voice(903, "HEADLESS")});
  write(f.syx / "headless.syx", std::vector<uint8_t>(e.begin() + 6, e.begin() + 6 + 4096));
  std::mt19937 rng(7);
  std::vector<uint8_t> noise(4096);
  for (auto& x : noise) x = static_cast<uint8_t>(rng() & 0x7F);
  write(f.syx / "noise.syx", noise);
  // A .snd with no F7, and a file that isn't sysex at all.
  auto snd = bulk({voice(904, "SND")});
  snd.pop_back();
  write(f.syx / "book.snd", snd);
  write(f.syx / "readme.syx", {'h', 'e', 'l', 'l', 'o'});
  write(f.syx / "notes.txt", {'x'});  // not looked at

  const ImportReport r = FmLibrary::import(f.db, f.syx, f.log);
  CHECK(r.files == 9);
  CHECK(r.corrupt == 2);     // bad.syx and noise.syx
  CHECK(r.unreadable == 1);  // readme.syx
  std::string error;
  auto lib = FmLibrary::open(f.db, error);
  REQUIRE(lib);
  for (const syx::Voice& v : c) CHECK(lib->findVoice(v));
  for (const char* name : {"D0", "SINGLE", "HEADLESS", "SND"}) {
    bool found = false;
    for (const FmPatch& p : lib->patches()) found = found || p.name == name;
    CHECK_MESSAGE(found, name);
  }
  for (const FmPatch& p : lib->patches()) CHECK(p.name != "BAD");

  std::ifstream in(f.log);
  const std::string log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(log.find("bad.syx: checksum mismatch") != std::string::npos);
  CHECK(log.find("noise.syx: 4096 bytes but not DX7 voice names") != std::string::npos);
  CHECK(log.find("readme.syx: no DX7 voice data") != std::string::npos);
}

TEST_CASE("library: junk never crashes the import") {
  Fixture f;
  std::mt19937 rng(99);
  const auto good = bulk(f.a);
  for (int i = 0; i < 60; ++i) {
    std::vector<uint8_t> bytes = good;
    if (i % 3 == 0) bytes.resize(rng() % bytes.size());
    for (int k = 0; k < 20; ++k) bytes[rng() % std::max<std::size_t>(1, bytes.size())] = static_cast<uint8_t>(rng());
    if (bytes.empty()) bytes.push_back(0xF0);
    write(f.syx / "junk" / ("j" + std::to_string(i) + ".syx"), bytes);
  }
  ImportReport r;
  CHECK_NOTHROW(r = FmLibrary::import(f.db, f.syx, f.log));
  CHECK(r.error.empty());
  std::string error;
  auto lib = FmLibrary::open(f.db, error);
  REQUIRE(lib);
  for (const FmPatch& p : lib->patches()) CHECK(syx::normalize(p.voice) == p.voice);
}

TEST_CASE("library: favourites, tags and recent survive reopening; filter finds them") {
  Fixture f;
  FmLibrary::import(f.db, f.syx, f.log);
  std::string error;
  int64_t id = 0;
  {
    auto lib = FmLibrary::open(f.db, error);
    REQUIRE(lib);
    id = lib->findVoice(f.a[5])->id;
    CHECK(lib->setFavourite(id, true));
    CHECK(lib->setTags(id, {"Garage", "bass", "garage", ""}));
    CHECK(lib->markUsed(id));
  }
  auto lib = FmLibrary::open(f.db, error);
  REQUIRE(lib);
  const FmPatch* p = lib->find(id);
  REQUIRE(p);
  CHECK(p->favourite);
  CHECK(p->tags == std::vector<std::string>{"bass", "garage"});
  CHECK(p->lastUsed > 0);

  auto names = [&](const std::vector<int>& idx) {
    std::vector<std::string> n;
    for (int i : idx) n.push_back(lib->patches()[static_cast<std::size_t>(i)].name);
    return n;
  };
  CHECK(names(lib->filter("", {}, true)) == std::vector<std::string>{"A5"});
  CHECK(names(lib->filter("gar")) == std::vector<std::string>{"A5"});         // tag prefix
  CHECK(names(lib->filter("a5 bass")) == std::vector<std::string>{"A5"});     // every word must match
  CHECK(lib->filter("a5 piano").empty());
  CHECK(lib->filter("bank_b").size() == 16);  // first source bank: the 16 not already in bank_a
  CHECK(names(lib->filter("", {}, false, true)) == std::vector<std::string>{"A5"});
  CHECK(lib->filter("A1").size() == 11);  // A1, A10 … A19: case-insensitive substring
}

TEST_CASE("library: a voice saved in ion is de-duplicated too") {
  Fixture f;
  FmLibrary::import(f.db, f.syx, f.log);
  std::string error;
  auto lib = FmLibrary::open(f.db, error);
  REQUIRE(lib);
  const int64_t existing = lib->findVoice(f.a[0])->id;
  CHECK(lib->addUser(renamed(f.a[0], "MY COPY"), {}, error) == existing);
  const int64_t id = lib->addUser(voice(4242, "MY BASS"), {"mine"}, error);
  REQUIRE(id > 0);
  const FmPatch* p = lib->find(id);
  REQUIRE(p);
  CHECK(p->user);
  CHECK(p->source == "saved");
  CHECK(p->category == "bass");
  CHECK(p->tags == std::vector<std::string>{"mine"});
}

TEST_CASE("library: exported banks load back as the same voices") {
  std::vector<syx::Voice> voices;
  for (uint32_t i = 0; i < 40; ++i) voices.push_back(voice(i + 7000, ("E" + std::to_string(i)).c_str()));
  const auto banks = syx::makeBanks(voices);
  REQUIRE(banks.size() == 2);
  std::vector<syx::Voice> back;
  for (const auto& bank : banks) {
    CHECK(bank.size() == syx::kBulkDumpSize);
    const syx::ParseResult r = syx::parse(bank);
    REQUIRE(r.dumps.size() == 1);
    CHECK(r.dumps[0].checksum_ok);
    for (const syx::Voice& v : syx::voices(r.dumps[0])) back.push_back(v);
  }
  for (std::size_t i = 0; i < voices.size(); ++i) CHECK(back[i] == voices[i]);
  for (std::size_t i = voices.size(); i < 64; ++i) CHECK(syx::name(back[i]) == "INIT VOICE");
}

TEST_CASE("library: rough categories from names") {
  const std::vector<std::pair<const char*, const char*>> cases = {
      {"E.PIANO 1", "keys"},  {"BASS    1", "bass"},   {"SYN-BAS 4", "bass"},   {"BRASS   1", "brass"},
      {"STRINGS 1", "strings"}, {"DEEP SPACE", "pad"}, {"TUB BELLS", "bell"},   {"KOTO", "pluck"},
      {"FLUTE   1", "wind"},  {"VOICE   1", "voice"},  {"SYN-LEAD 1", "lead"},  {"E.ORGAN 1", "organ"},
      {"TIMPANI", "perc"},    {"TAKE OFF", "fx"},      {"PANPIPES", "wind"},    {"DEEP", "other"},
      {"HighRhod", "keys"},   {"STEEL DRUM", "perc"},
  };
  for (const auto& [name, category] : cases) CHECK(fmCategory(name) + " ← " + name == std::string(category) + " ← " + name);
}
