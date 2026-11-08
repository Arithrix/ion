#include "ui/sample_browse.h"

#include <algorithm>
#include <cctype>

#include "project/paths.h"
#include "project/session.h"

namespace ion::ui {
namespace fs = std::filesystem;

bool naturalLess(const std::string& a, const std::string& b) {
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const auto ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
    if (std::isdigit(ca) && std::isdigit(cb)) {
      std::size_t ei = i, ej = j;
      while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
      while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
      // Compare the numbers by value: strip leading zeros, then length, then digits.
      std::size_t si = i, sj = j;
      while (si + 1 < ei && a[si] == '0') ++si;
      while (sj + 1 < ej && b[sj] == '0') ++sj;
      if (ei - si != ej - sj) return ei - si < ej - sj;
      if (const int c = a.compare(si, ei - si, b, sj, ej - sj); c != 0) return c < 0;
      i = ei;
      j = ej;
      continue;
    }
    const int la = std::tolower(ca), lb = std::tolower(cb);
    if (la != lb) return la < lb;
    ++i;
    ++j;
  }
  return a.size() - i < b.size() - j;
}

bool SampleBrowse::isRoot(const Session& s, const fs::path& p) const {
  if (p == s.paths().kitsDir || p == expandHome("~")) return true;
  if (!s.recordingsDir().empty() && p == s.recordingsDir()) return true;
  for (const std::string& r : s.config().sampleRoots)
    if (expandHome(r) == p) return true;
  return false;
}

void SampleBrowse::refresh(const Session& s) {
  all_.clear();
  std::error_code ec;
  if (top_) {
    for (const std::string& r : s.config().sampleRoots) {
      const fs::path p = expandHome(r);
      Entry e{Entry::Kind::Root, abbreviateHome(p.string()) + "/", p};
      e.missing = !fs::is_directory(p, ec);
      all_.push_back(e);
    }
    // This project's takes (Rec screen), once there are any.
    if (const fs::path rec = s.recordingsDir(); !rec.empty() && fs::is_directory(rec, ec))
      all_.push_back({Entry::Kind::Root, "recordings (this project)", rec});
    all_.push_back({Entry::Kind::Root, "drum kits", s.paths().kitsDir});
    all_.push_back({Entry::Kind::Root, "~/ (home)", expandHome("~")});
  } else {
    all_.push_back({Entry::Kind::Parent, "../", dir_.parent_path()});
    const bool kits = dir_ == s.paths().kitsDir;
    std::vector<Entry> folders, files;
    for (const auto& d : fs::directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec)) {
      const std::string name = d.path().filename().string();
      if (name.empty() || name[0] == '.') continue;
      if (d.is_directory(ec)) folders.push_back({Entry::Kind::Folder, name + "/", d.path()});
      else if (kits && d.path().extension() == ".json") files.push_back({Entry::Kind::Kit, d.path().stem().string(), d.path()});
      else if (isAudioFile(d.path())) files.push_back({Entry::Kind::Audio, name, d.path()});
    }
    auto byName = [](const Entry& x, const Entry& y) { return naturalLess(x.name, y.name); };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    all_.insert(all_.end(), folders.begin(), folders.end());
    all_.insert(all_.end(), files.begin(), files.end());
    stamp_ = fs::last_write_time(dir_, ec);
  }
  stale_ = false;
  builtFilter_ = "\x01";  // rebuild shown_
}

const std::vector<SampleBrowse::Entry>& SampleBrowse::entries(const Session& s) {
  std::error_code ec;
  if (!stale_ && !top_ && fs::last_write_time(dir_, ec) != stamp_) stale_ = true;
  if (stale_) refresh(s);
  if (builtFilter_ != filter) {
    builtFilter_ = filter;
    std::string f = filter;
    for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    shown_.clear();
    for (const Entry& e : all_) {
      std::string n = e.name;
      for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (f.empty() || e.kind == Entry::Kind::Parent || n.find(f) != std::string::npos) shown_.push_back(e);
    }
    // Typing a filter lands on the first match, not on ../
    if (jumpFirst_) cursor_ = shown_.size() > 1 && shown_[0].kind == Entry::Kind::Parent ? 1 : 0;
    jumpFirst_ = false;
  }
  return shown_;
}

int SampleBrowse::cursor(const Session& s) {
  const int n = static_cast<int>(entries(s).size());
  cursor_ = n == 0 ? 0 : std::clamp(cursor_, 0, n - 1);
  return cursor_;
}

void SampleBrowse::move(const Session& s, int delta) { cursor_ = std::clamp(cursor(s) + delta, 0, std::max(0, static_cast<int>(entries(s).size()) - 1)); }

const SampleBrowse::Entry* SampleBrowse::current(const Session& s) {
  const auto& e = entries(s);
  return e.empty() ? nullptr : &e[static_cast<std::size_t>(cursor(s))];
}

bool SampleBrowse::open(const Session& s, const Entry& e) {
  if (e.kind == Entry::Kind::Parent) {
    up(s);
    return true;
  }
  if (e.kind != Entry::Kind::Root && e.kind != Entry::Kind::Folder) return false;
  std::error_code ec;
  if (!fs::is_directory(e.path, ec)) return false;
  remembered_[top_ ? fs::path() : dir_] = cursor_;
  top_ = false;
  dir_ = e.path;
  filter.clear();
  typing = false;
  stale_ = true;
  cursor_ = 1;  // the first entry after ../
  if (auto it = remembered_.find(dir_); it != remembered_.end()) cursor_ = it->second;
  return true;
}

void SampleBrowse::up(const Session& s) {
  if (top_) return;
  const fs::path from = dir_;
  remembered_[dir_] = cursor_;
  filter.clear();
  typing = false;
  stale_ = true;
  // Up from a root goes back to the top level; elsewhere to the parent folder.
  if (isRoot(s, dir_) || dir_ == dir_.root_path()) {
    top_ = true;
    dir_.clear();
  } else {
    dir_ = dir_.parent_path();
  }
  // Land on the folder we came out of.
  cursor_ = 0;
  for (std::size_t i = 0; i < entries(s).size(); ++i)
    if (shown_[i].path == from) cursor_ = static_cast<int>(i);
}

void SampleBrowse::show(const Session& s, const fs::path& folder, const fs::path& file) {
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return;
  if (!top_) remembered_[dir_] = cursor_;
  top_ = false;
  dir_ = folder;
  filter.clear();
  typing = false;
  stale_ = true;
  cursor_ = 1;
  const auto& list = entries(s);
  for (std::size_t i = 0; i < list.size(); ++i)
    if (!file.empty() && list[i].path == file) cursor_ = static_cast<int>(i);
}

std::string SampleBrowse::location() const {
  if (top_) return "sample roots";
  std::string p = abbreviateHome(dir_.string());
  if (p.empty() || p.back() != '/') p += '/';
  return p;
}

void SampleBrowse::setFilter(std::string f) {
  filter = std::move(f);
  jumpFirst_ = !filter.empty();
}

const AudioFileInfo& SampleBrowse::info(const fs::path& path) {
  auto it = info_.find(path);
  if (it == info_.end()) {
    if (info_.size() > 4096) info_.clear();
    it = info_.emplace(path, probeAudioFile(path)).first;
  }
  return it->second;
}

}  // namespace ion::ui
