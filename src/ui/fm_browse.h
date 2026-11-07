#pragma once

// What the FM library browser is showing: the filter, the view (all,
// favourites, a category…), the list that results, and where each track's
// cursor is. Shared by the Instrument screen and the Patch row on Tracks, so
// ◀ ▶ there steps through the same list.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/params.h"

namespace ion {
class Session;
struct FmPatch;
}  // namespace ion

namespace ion::ui {

class FmBrowse {
 public:
  struct View {
    enum class Kind { All, Favourites, Recent, Programs, Category } kind;
    std::string label;     // "all", "★ favourites", "bass"…
    std::string category;  // for Kind::Category
  };
  static const std::vector<View>& views();

  // One row: a library patch, or (in the Programs view) an entry of the
  // project's program list, which may not be in this machine's library.
  struct Item {
    const FmPatch* patch = nullptr;
    int program = -1;
  };

  std::string filter;
  bool typing = false;  // keys go into the filter
  int view = 0;

  // The rows for the current filter and view. Recomputed only when something
  // they depend on changed.
  const std::vector<Item>& items(const Session& s);
  // The highlighted row for track t: its current patch if listed, otherwise
  // where the cursor was last. -1 if the list is empty.
  int cursor(const Session& s, int t);
  // Moves the cursor and loads the patch there (HANDOFF §6.3: "moving the
  // cursor loads the patch onto the track immediately"). False at either end.
  bool move(Session& s, int t, int delta);
  void loadAt(Session& s, int t, int index);
  void setView(int v);
  void setFilter(std::string f);

  std::string name(const Session& s, const Item& item) const;
  const uint8_t* voice(const Session& s, const Item& item) const;  // 155 bytes

 private:
  std::vector<Item> items_;
  uint64_t generation_ = ~0ull;
  std::size_t programs_ = ~std::size_t{0};
  std::string builtFilter_;
  int builtView_ = -1;
  std::array<int, kTrackCount> rows_{};
};

}  // namespace ion::ui
