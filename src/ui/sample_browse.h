#pragma once

// The sample browser's state (mockups/DrumKit, HANDOFF §6.1: "the sample
// browser is the left pane, Midnight Commander style"). The top level lists
// the sample roots from the config, the kits folder and home; below that it's
// plain folders and audio files.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "library/sample_loader.h"

namespace ion {
class Session;
}

namespace ion::ui {

class SampleBrowse {
 public:
  struct Entry {
    enum class Kind { Root, Parent, Folder, Audio, Kit } kind;
    std::string name;  // as shown
    std::filesystem::path path;
    bool missing = false;  // a root that doesn't exist
  };

  // The rows for the current folder and filter (re-read when the folder changes on disk).
  const std::vector<Entry>& entries(const Session& s);
  int cursor(const Session& s);  // clamped
  void move(const Session& s, int delta);
  const Entry* current(const Session& s);

  // Into a folder (Root/Folder), or up (Parent). Returns false for files.
  bool open(const Session& s, const Entry& e);
  void up(const Session& s);
  // Straight to `folder` (if it exists), with the cursor on `file` if given.
  void show(const Session& s, const std::filesystem::path& folder, const std::filesystem::path& file = {});
  bool atTop() const { return top_; }
  // "~/samples/garage/kicks/", or "sample roots".
  std::string location() const;

  std::string filter;
  bool typing = false;
  void setFilter(std::string f);

  // From the file's header, read once.
  const AudioFileInfo& info(const std::filesystem::path& path);

 private:
  void refresh(const Session& s);
  bool isRoot(const Session& s, const std::filesystem::path& p) const;

  bool top_ = true;
  std::filesystem::path dir_;
  std::vector<Entry> all_, shown_;
  bool stale_ = true;
  std::filesystem::file_time_type stamp_{};
  std::string builtFilter_;
  int cursor_ = 0;
  bool jumpFirst_ = false;
  std::map<std::filesystem::path, int> remembered_;  // cursor per folder, for coming back up
  std::map<std::filesystem::path, AudioFileInfo> info_;
};

// "kick 2" before "kick 10", case ignored.
bool naturalLess(const std::string& a, const std::string& b);

}  // namespace ion::ui
