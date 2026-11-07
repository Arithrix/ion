#include "ui/fm_browse.h"

#include <algorithm>
#include <cctype>

#include "library/fm_library.h"
#include "project/session.h"

namespace ion::ui {

const std::vector<FmBrowse::View>& FmBrowse::views() {
  static const std::vector<View> v = [] {
    std::vector<View> out = {{View::Kind::All, "all", ""},
                             {View::Kind::Favourites, "★ favourites", ""},
                             {View::Kind::Recent, "recent", ""},
                             {View::Kind::Programs, "program list", ""}};
    for (const char* c : fmCategories()) out.push_back({View::Kind::Category, c, c});
    return out;
  }();
  return v;
}

void FmBrowse::setView(int v) {
  const int n = static_cast<int>(views().size());
  view = ((v % n) + n) % n;
  rows_.fill(0);
}

void FmBrowse::setFilter(std::string f) {
  filter = std::move(f);
  rows_.fill(0);
}

const std::vector<FmBrowse::Item>& FmBrowse::items(const Session& s) {
  const FmLibrary* lib = s.library();
  const uint64_t gen = lib ? lib->generation() : 0;
  if (gen == generation_ && builtFilter_ == filter && builtView_ == view && programs_ == s.programs().size())
    return items_;
  generation_ = gen;
  builtFilter_ = filter;
  builtView_ = view;
  programs_ = s.programs().size();
  items_.clear();

  const View& v = views()[static_cast<std::size_t>(view)];
  if (v.kind == View::Kind::Programs) {
    std::string f = filter;
    for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i < static_cast<int>(s.programs().size()); ++i) {
      Item it;
      it.program = i;
      it.patch = lib ? lib->findVoice(s.programs()[static_cast<std::size_t>(i)].voice) : nullptr;
      std::string n = name(s, it);
      for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (f.empty() || n.find(f) != std::string::npos) items_.push_back(it);
    }
    return items_;
  }
  if (!lib) return items_;
  const std::vector<int> idx = lib->filter(filter, v.category, v.kind == View::Kind::Favourites, v.kind == View::Kind::Recent);
  items_.reserve(idx.size());
  for (int i : idx) items_.push_back({&lib->patches()[static_cast<std::size_t>(i)], -1});
  return items_;
}

int FmBrowse::cursor(const Session& s, int t) {
  const auto& list = items(s);
  if (list.empty()) return -1;
  const TrackState& tr = s.track(t);
  const int remembered = std::clamp(rows_[static_cast<std::size_t>(t)], 0, static_cast<int>(list.size()) - 1);
  auto matches = [&](const Item& it) {
    if (it.program >= 0) return tr.hasVoice && s.programIndex(tr.voice) == it.program;
    return tr.libraryId != 0 && it.patch && it.patch->id == tr.libraryId;
  };
  // Prefer the remembered row when it still holds the track's patch (the same
  // voice can be in the program list twice… or a filter can list it once).
  if (matches(list[static_cast<std::size_t>(remembered)])) return remembered;
  for (std::size_t i = 0; i < list.size(); ++i)
    if (matches(list[i])) return static_cast<int>(i);
  return remembered;
}

bool FmBrowse::move(Session& s, int t, int delta) {
  const auto& list = items(s);
  if (list.empty()) return false;
  const int from = cursor(s, t);
  const int to = std::clamp(from + delta, 0, static_cast<int>(list.size()) - 1);
  // If the track's patch isn't on the list, the first press loads the
  // highlighted row rather than skipping past it.
  const TrackState& tr = s.track(t);
  const Item& here = list[static_cast<std::size_t>(from)];
  const bool loaded = here.program >= 0 ? (tr.hasVoice && s.programIndex(tr.voice) == here.program)
                                        : (here.patch && here.patch->id == tr.libraryId);
  const int target = loaded ? to : from;
  if (loaded && target == from) return false;
  loadAt(s, t, target);
  return true;
}

void FmBrowse::loadAt(Session& s, int t, int index) {
  const auto& list = items(s);
  if (index < 0 || index >= static_cast<int>(list.size())) return;
  rows_[static_cast<std::size_t>(t)] = index;
  const Item& it = list[static_cast<std::size_t>(index)];
  if (it.patch && it.program < 0) {
    s.loadPatch(t, *it.patch);
  } else if (it.program >= 0) {
    const ProgramEntry& p = s.programs()[static_cast<std::size_t>(it.program)];
    s.setVoice(t, p.voice, p.source);
  }
}

std::string FmBrowse::name(const Session& s, const Item& item) const {
  std::string n;
  if (item.program >= 0) n = syx::name(s.programs()[static_cast<std::size_t>(item.program)].voice);
  else if (item.patch) n = item.patch->name;
  return n.empty() ? "(no name)" : n;
}

const uint8_t* FmBrowse::voice(const Session& s, const Item& item) const {
  if (item.program >= 0) return s.programs()[static_cast<std::size_t>(item.program)].voice.data();
  return item.patch ? item.patch->voice.data() : nullptr;
}

}  // namespace ion::ui
