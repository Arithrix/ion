#include "ui/algorithm_diagram.h"

#include <algorithm>
#include <array>

#include "engine/fm/dx7_voice.h"

namespace ion::ui {
namespace {

// Connection bits for line cells; the glyph is chosen from the combination.
enum : uint8_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8 };

const char* lineGlyph(uint8_t bits) {
  switch (bits) {
    case kUp | kDown: return "│";
    case kLeft | kRight: return "─";
    case kUp | kRight: return "└";
    case kUp | kLeft: return "┘";
    case kDown | kRight: return "┌";
    case kDown | kLeft: return "┐";
    case kUp | kDown | kRight: return "├";
    case kUp | kDown | kLeft: return "┤";
    case kUp | kLeft | kRight: return "┴";
    case kDown | kLeft | kRight: return "┬";
    case kUp | kDown | kLeft | kRight: return "┼";
    case kUp: case kDown: return "│";
    case kLeft: case kRight: return "─";
    default: return " ";
  }
}

struct Canvas {
  int width, height;
  std::vector<uint8_t> bits;
  std::vector<std::string> fixed;  // a literal glyph, which wins over bits

  Canvas(int w, int h) : width(w), height(h), bits(static_cast<std::size_t>(w * h), 0), fixed(static_cast<std::size_t>(w * h)) {}
  std::size_t at(int x, int y) const { return static_cast<std::size_t>(y * width + x); }
  void put(int x, int y, const char* glyph) { fixed[at(x, y)] = glyph; }
  void link(int x, int y, uint8_t b) { bits[at(x, y)] |= b; }
  // A horizontal run on row y between x0 and x1 (either order).
  void hline(int y, int x0, int x1) {
    if (x0 > x1) std::swap(x0, x1);
    for (int x = x0; x <= x1; ++x) link(x, y, static_cast<uint8_t>((x > x0 ? kLeft : 0) | (x < x1 ? kRight : 0)));
  }
  std::vector<std::string> lines() const {
    std::vector<std::string> out;
    for (int y = 0; y < height; ++y) {
      std::string s;
      for (int x = 0; x < width; ++x) s += !fixed[at(x, y)].empty() ? fixed[at(x, y)] : lineGlyph(bits[at(x, y)]);
      out.push_back(s);
    }
    return out;
  }
};

struct Layout {
  std::array<int, 7> col{}, row{};  // per op 1-6
  std::array<uint8_t, 7> targets{};  // bitmask of ops each op modulates
  int cols = 0, rows = 0;
};

Layout layout(const dx7::Topology& t) {
  Layout L;
  for (int op = 1; op <= 6; ++op)
    for (int m = 1; m <= 6; ++m)
      if (t.modulators[static_cast<std::size_t>(op)] & (1u << m)) L.targets[static_cast<std::size_t>(m)] |= static_cast<uint8_t>(1u << op);
  auto fanOut = [&](int op) { return __builtin_popcount(L.targets[static_cast<std::size_t>(op)]) > 1; };

  // Depth: carriers 0, each modulator one above what it modulates.
  std::array<int, 7> depth{};
  depth.fill(-1);
  int next = 0, maxDepth = 0;
  // Leaves take the next free column; a node sits under its first modulator.
  auto place = [&](auto&& self, int op, int d) -> void {
    depth[static_cast<std::size_t>(op)] = d;
    maxDepth = std::max(maxDepth, d);
    bool first = true;
    for (int m = 1; m <= 6; ++m) {
      if (!(t.modulators[static_cast<std::size_t>(op)] & (1u << m)) || fanOut(m)) continue;
      self(self, m, d + 1);
      if (first) L.col[static_cast<std::size_t>(op)] = L.col[static_cast<std::size_t>(m)];
      first = false;
    }
    if (first) L.col[static_cast<std::size_t>(op)] = next++;
  };
  for (int op = 1; op <= 6; ++op)
    if (t.carrier(op)) place(place, op, 0);
  // A modulator feeding several operators sits above the middle of them.
  for (int op = 1; op <= 6; ++op) {
    if (!fanOut(op)) continue;
    int lo = 99, hi = -1, d = 0;
    for (int x = 1; x <= 6; ++x)
      if (L.targets[static_cast<std::size_t>(op)] & (1u << x)) {
        lo = std::min(lo, L.col[static_cast<std::size_t>(x)]);
        hi = std::max(hi, L.col[static_cast<std::size_t>(x)]);
        d = std::max(d, depth[static_cast<std::size_t>(x)] + 1);
      }
    L.col[static_cast<std::size_t>(op)] = (lo + hi) / 2;
    depth[static_cast<std::size_t>(op)] = d;
    maxDepth = std::max(maxDepth, d);
  }
  L.cols = next;
  L.rows = maxDepth + 1;
  for (int op = 1; op <= 6; ++op) L.row[static_cast<std::size_t>(op)] = maxDepth - depth[static_cast<std::size_t>(op)];
  return L;
}

}  // namespace

std::vector<std::string> algorithmDiagram(int algorithm, bool boxed) {
  const dx7::Topology& t = dx7::topology(algorithm);
  const Layout L = layout(t);
  const int pitch = boxed ? 5 : 3;       // columns apart, leaving room for ↺
  const int boxH = boxed ? 3 : 1;
  int carriers = 0;
  for (int op = 1; op <= 6; ++op) carriers += t.carrier(op);

  // Boxed rows touch when every link between them is a straight drop;
  // otherwise a connector line goes in between. Compact always has one.
  std::vector<bool> gapAfter(static_cast<std::size_t>(L.rows), !boxed);
  for (int m = 1; m <= 6; ++m)
    for (int x = 1; x <= 6; ++x)
      if (L.targets[static_cast<std::size_t>(m)] & (1u << x)) {
        const bool straight = L.col[static_cast<std::size_t>(m)] == L.col[static_cast<std::size_t>(x)] &&
                              __builtin_popcount(L.targets[static_cast<std::size_t>(m)]) == 1 &&
                              __builtin_popcount(t.modulators[static_cast<std::size_t>(x)]) == 1;
        if (!straight) gapAfter[static_cast<std::size_t>(L.row[static_cast<std::size_t>(m)])] = true;
      }
  std::vector<int> rowY(static_cast<std::size_t>(L.rows));
  int y = 0;
  for (int r = 0; r < L.rows; ++r) {
    rowY[static_cast<std::size_t>(r)] = y;
    y += boxH + ((r < L.rows - 1 && gapAfter[static_cast<std::size_t>(r)]) ? 1 : 0);
  }
  const int busY = y;  // below the carriers
  Canvas c((L.cols - 1) * pitch + (boxed ? 3 : 1) + 1, busY + (carriers > 1 ? 1 : 0));

  auto px = [&](int op) { return L.col[static_cast<std::size_t>(op)] * pitch; };
  auto py = [&](int op) { return rowY[static_cast<std::size_t>(L.row[static_cast<std::size_t>(op)])]; };
  auto centre = [&](int op) { return px(op) + (boxed ? 1 : 0); };
  static const char* digits[] = {"0", "1", "2", "3", "4", "5", "6"};

  for (int op = 1; op <= 6; ++op) {
    const int x = px(op), top = py(op);
    const bool hasInput = t.modulators[static_cast<std::size_t>(op)] != 0;
    const bool hasOutput = L.targets[static_cast<std::size_t>(op)] != 0 || (t.carrier(op) && carriers > 1);
    if (boxed) {
      c.put(x, top, "┌"); c.put(x + 1, top, hasInput ? "┴" : "─"); c.put(x + 2, top, "┐");
      c.put(x, top + 1, "│"); c.put(x + 1, top + 1, digits[op]); c.put(x + 2, top + 1, "│");
      c.put(x, top + 2, "└"); c.put(x + 1, top + 2, hasOutput ? "┬" : "─"); c.put(x + 2, top + 2, "┘");
    } else {
      c.put(x, top, digits[op]);
    }
    if (op == t.feedbackTo) c.put(x + (boxed ? 3 : 1), top, "↺");
  }

  // Links: down from the modulator, along the connector line, down into the target.
  for (int m = 1; m <= 6; ++m)
    for (int x = 1; x <= 6; ++x) {
      if (!(L.targets[static_cast<std::size_t>(m)] & (1u << x))) continue;
      const int bottom = py(m) + boxH - 1;  // last line of the modulator
      const int gap = bottom + 1;
      if (gap >= py(x)) continue;  // boxes touching: ┬ meets ┴
      c.link(centre(m), gap, kUp);
      c.link(centre(x), gap, kDown);
      c.hline(gap, centre(m), centre(x));
    }
  // The output line joining the carriers.
  if (carriers > 1) {
    int lo = 1 << 30, hi = -1;
    for (int op = 1; op <= 6; ++op)
      if (t.carrier(op)) {
        c.link(centre(op), busY, kUp);
        lo = std::min(lo, centre(op));
        hi = std::max(hi, centre(op));
      }
    c.hline(busY, lo, hi);
  }
  return c.lines();
}

}  // namespace ion::ui
