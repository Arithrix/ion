#pragma once

// The FM library (HANDOFF §6.3): every DX7 voice ever imported, de-duplicated,
// in one flat list. No banks in the UI; a voice remembers which banks it came
// from ("from cart_17").
//
// Stored in SQLite at ~/.local/share/ion/library.db. The whole list is small
// (tens of thousands of voices are a few MB), so it's read into memory once and
// filtered there as you type.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "library/syx.h"

struct sqlite3;

namespace ion {

struct FmPatch {
  int64_t id = 0;
  std::string name;         // the name it was first seen with
  syx::Voice voice{};       // normalised
  int algorithm = 0;        // 0-31
  std::string category;     // rough, from the name: "bass", "keys", … or "other"
  std::string source;       // first bank it came from (file name without extension), or "saved"
  int sourceCount = 0;      // how many banks it was found in
  std::vector<std::string> tags;
  bool favourite = false;
  int64_t lastUsed = 0;     // unix time, 0 = never
  bool user = false;        // made in ion with "save as"
};

struct ImportProgress {
  std::atomic<int> filesTotal{0};
  std::atomic<int> filesDone{0};
  std::atomic<bool> cancel{false};
};

struct ImportReport {
  int files = 0;       // .syx/.snd files looked at
  int banks = 0;       // voice dumps read (bulk or single)
  int voices = 0;      // voices in them
  int added = 0;       // new to the library
  int corrupt = 0;     // dumps skipped: bad checksum, truncated, not voice data
  int unreadable = 0;  // files with no voice data at all, or that couldn't be read
  std::vector<std::string> problems;  // one line each, also written to import.log
  std::string error;   // set if the import couldn't run at all
};

// Categories the browser can filter by, in display order.
std::span<const char* const> fmCategories();
// The rough category for a patch name.
std::string fmCategory(const std::string& name);
// Identity for de-duplication: every parameter except the name.
uint64_t fmVoiceHash(const syx::Voice& voice);

class FmLibrary {
 public:
  ~FmLibrary();
  // Creates the database if needed. Null (with `error`) if it can't be opened.
  static std::unique_ptr<FmLibrary> open(const std::filesystem::path& dbPath, std::string& error);

  // Imports every .syx / .snd file under `folder` (or the one file). Safe to
  // run on a worker thread while the UI keeps using this library: it opens its
  // own connection. Call reload() afterwards. Never throws on bad files.
  static ImportReport import(const std::filesystem::path& dbPath, const std::filesystem::path& folder,
                             const std::filesystem::path& logPath, ImportProgress* progress = nullptr);

  // Re-reads everything (after an import).
  bool reload(std::string& error);

  const std::vector<FmPatch>& patches() const { return patches_; }
  // Changes whenever anything in patches() does (pointers into it may then be stale).
  uint64_t generation() const { return generation_; }
  const FmPatch* find(int64_t id) const;
  const FmPatch* findVoice(const syx::Voice& voice) const;  // by hash, ignoring the name

  // Patches matching a filter: words separated by spaces, all of which must
  // appear in the name, a tag, the category or a source bank. Indices into
  // patches(), in name order.
  std::vector<int> filter(const std::string& text, const std::string& category = {}, bool favouritesOnly = false,
                          bool recentOnly = false) const;

  bool setFavourite(int64_t id, bool on);
  bool setTags(int64_t id, const std::vector<std::string>& tags);
  bool markUsed(int64_t id);
  // Every bank the patch was found in ("cart_17", "rom1a", …).
  std::vector<std::string> sources(int64_t id) const;

  // Adds a voice made in ion (a baked "save as"). If an identical voice is
  // already in the library, that one is returned instead. 0 on failure.
  int64_t addUser(const syx::Voice& voice, const std::vector<std::string>& tags, std::string& error);

  // The last import, for the Library screen: "412 banks → 13,184 voices → 2,340 unique".
  struct LastImport {
    int64_t time = 0;
    std::string folder;
    int banks = 0, voices = 0, added = 0, corrupt = 0;
  };
  std::optional<LastImport> lastImport() const;

 private:
  FmLibrary() = default;
  void sortPatches();

  sqlite3* db_ = nullptr;
  std::vector<FmPatch> patches_;
  std::unordered_map<uint64_t, std::size_t> byHash_;  // fmVoiceHash → index in patches_
  std::unordered_map<int64_t, std::size_t> byId_;
  uint64_t generation_ = 0;
};

}  // namespace ion
