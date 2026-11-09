// Mod (O): the selected track's modulation (DECISIONS D-58). Left, its two
// LFOs and two envelopes; right, its eight routes: a source, a target (any
// continuous value on the track) and a depth. `v` on any value elsewhere
// adds a route onto it.

#include <cmath>
#include <cstdio>
#include <numbers>

#include "engine/envelope_params.h"
#include "engine/module.h"
#include "project/session.h"
#include "ui/envelope_view.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;

constexpr int kModulatorRows = kLfos * kLfoParamCount + kModEnvs * kModEnvParamCount;  // 24

ParamRef modulatorRef(int t, int row) {
  if (row < kLfos * kLfoParamCount) return lfoRef(t, row / kLfoParamCount, row % kLfoParamCount);
  const int e = row - kLfos * kLfoParamCount;
  return modEnvRef(t, e / kModEnvParamCount, e % kModEnvParamCount);
}

// A bipolar value as a bar centred on zero.
Element centred(float v, int width, Color c) {
  const int half = width / 2;
  const int n = static_cast<int>(std::lround(std::min(1.0f, std::fabs(v)) * static_cast<float>(half)));
  std::string left, right;
  for (int i = 0; i < half; ++i) left += (v < 0 && i >= half - n) ? "█" : "░";
  for (int i = 0; i < width - half; ++i) right += (v > 0 && i < n) ? "█" : "░";
  return hbox({text(left) | color(v < 0 ? c : palette::border), text(right) | color(v > 0 ? c : palette::border)});
}

// An LFO's shape over one cycle, in block characters.
std::string lfoShapeText(int shape, int width) {
  static const char* levels[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  std::string out;
  uint32_t seed = 7;
  float held = 0;
  for (int x = 0; x < width; ++x) {
    const double p = (x + 0.5) / width;
    float v = 0;
    switch (shape) {
      case kLfoSine: v = static_cast<float>(std::sin(2 * std::numbers::pi * p)); break;
      case kLfoTriangle: v = static_cast<float>(p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4); break;
      case kLfoSawUp: v = static_cast<float>(2 * p - 1); break;
      case kLfoSawDown: v = static_cast<float>(1 - 2 * p); break;
      case kLfoSquare: v = p < 0.5 ? 1.0f : -1.0f; break;
      default:  // S&H and smooth: a few steps of something random-looking
        if (x % std::max(1, width / 4) == 0) {
          seed = seed * 1664525u + 1013904223u;
          held = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
        }
        v = held;
    }
    out += levels[std::clamp(static_cast<int>(std::lround((v + 1) * 3.5f)), 0, 7)];
  }
  return out;
}

class ModScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    if (!ctx.wide()) return (pane_ == 0 ? modulators(ctx, ctx.width) : routes(ctx, ctx.width)) | flex;
    const int left = std::clamp(ctx.width * 48 / 100, 56, 70);
    return hbox({modulators(ctx, left) | size(WIDTH, EQUAL, left), routes(ctx, ctx.width - left) | flex});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    const int t = ctx.track;
    if (picking_) return pickerEvent(ctx, e);
    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = 1 - pane_;
      return true;
    }
    if (pane_ == 0) {
      if (e == Event::ArrowUp) { row_ = (row_ + kModulatorRows - 1) % kModulatorRows; return true; }
      if (e == Event::ArrowDown) { row_ = (row_ + 1) % kModulatorRows; return true; }
      if (const Nudge n = nudgeOf(e); n.direction) {
        s.nudgeParam(modulatorRef(t, row_), n.direction, n.coarse);
        return true;
      }
      return false;
    }
    if (e == Event::ArrowUp) { route_ = (route_ + kModRoutes - 1) % kModRoutes; return true; }
    if (e == Event::ArrowDown) { route_ = (route_ + 1) % kModRoutes; return true; }
    if (const Nudge n = nudgeOf(e); n.direction) {
      s.nudgeParam(routeRef(t, route_, kRouteDepth), n.direction, n.coarse);
      return true;
    }
    if (e == Event::Character('[') || e == Event::Character(']')) {
      s.nudgeParam(routeRef(t, route_, kRouteSource), e == Event::Character(']') ? 1 : -1, false);
      return true;
    }
    if (e == Event::Return) {
      openPicker(ctx);
      return true;
    }
    if (e == Event::Character('x')) {
      if (s.modTarget(t, route_)) {
        s.setModTarget(t, route_, std::nullopt);
        ctx.flash = "route " + std::to_string(route_ + 1) + " cleared";
      }
      return true;
    }
    return false;
  }

  Hints hints(Context& ctx) override {
    if (picking_) return {{"type", "Filter"}, {"↑↓", "Value"}, {"⏎", "Choose"}, {"Esc", "Cancel"}};
    if (pane_ == 0) return {{"↑↓", "Row"}, {"←→", "Change"}, {"l", "Learn"}, {"Tab", "Routes"}, {"1-8", "Track"}, {"Esc", "Back"}};
    Hints h = {{"↑↓", "Route"}, {"⏎", "Target"}, {"[ ]", "Source"}, {"←→", "Depth"}, {"x", "Clear"}, {"Tab", "Modulators"}};
    if (!ctx.narrow()) h.push_back({"l", "Learn depth"});
    h.push_back({"Esc", "Back"});
    return h;
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    if (picking_) return std::nullopt;
    if (pane_ == 0) return modulatorRef(ctx.track, row_);
    return routeRef(ctx.track, route_, kRouteDepth);
  }

  bool capturesText(Context&) override { return picking_; }

 private:
  // ---- left: the LFOs and envelopes ----

  Element modulators(Context& ctx, int width) {
    const Session& s = ctx.session;
    const int t = ctx.track;
    const bool focused = pane_ == 0 && !picking_;
    const int inner = std::max(24, width - 4);
    const bool twoColumns = inner >= 50;
    const int colW = twoColumns ? (inner - 2) / 2 : inner;
    const ModuleStatus& st = ctx.module.status();

    auto row = [&](int r, int w) {
      const ParamRef ref = modulatorRef(t, r);
      const ParamInfo* pi = s.paramInfo(ref);
      const bool on = focused && r == row_;
      // A synced LFO shows its division, not its rate; a free one the other way round.
      const int p = r < kLfos * kLfoParamCount ? r % kLfoParamCount : -1;
      const bool synced = p >= 0 && s.param(lfoRef(t, r / kLfoParamCount, kLfoSync)) >= 0.5f;
      const bool idle = (p == kLfoRate && synced) || (p == kLfoDivision && !synced);
      Element e = hbox({text(on ? "▸ " : "  ") | color(palette::accent),
                        text(fit(pi ? pi->label : "?", 9)) | size(WIDTH, EQUAL, 9) |
                            color(on ? palette::accent : idle ? palette::faint : palette::text),
                        text(" " + (pi ? formatValue(*pi, s.param(ref)) : "?")) | color(on ? palette::text : idle ? palette::faint : palette::dim),
                        filler()});
      (void)w;
      return ctx.value(on ? e | bgcolor(palette::raised) : e, [this, r](Context&) {
        pane_ = 0;
        row_ = r;
      });
    };
    auto group = [&](int g, int w) {
      Elements out;
      const bool isLfo = g < kLfos;
      const float now = st.modSources[static_cast<std::size_t>(t)][static_cast<std::size_t>(g)].load(std::memory_order_relaxed);
      const int glyphW = std::max(6, w - 18);
      if (isLfo) {
        const int shape = static_cast<int>(std::lround(s.param(lfoRef(t, g, kLfoShape))));
        out.push_back(hbox({text("LFO " + std::to_string(g + 1) + " ") | bold | color(palette::lilac),
                            text(lfoShapeText(shape, glyphW)) | color(palette::value), text(" "), centred(now, 6, palette::lilac)}));
      } else {
        const int e = g - kLfos;
        std::array<float, kEnvParamCount> env{};
        for (int k = 0; k < kEnvParamCount; ++k) env[static_cast<std::size_t>(k)] = s.param(modEnvRef(t, e, k));
        out.push_back(hbox({text("env " + std::to_string(e + 1) + " ") | bold | color(palette::lilac),
                            text(envelopeShape(env.data(), glyphW)) | color(palette::value), text(" "),
                            valueBar(now, 6, palette::lilac)}));
      }
      const int first = isLfo ? g * kLfoParamCount : kLfos * kLfoParamCount + (g - kLfos) * kModEnvParamCount;
      const int count = isLfo ? static_cast<int>(kLfoParamCount) : static_cast<int>(kModEnvParamCount);
      for (int r = first; r < first + count; ++r) out.push_back(row(r, w));
      return out;
    };

    Elements lines;
    if (twoColumns) {
      for (int pair = 0; pair < 2; ++pair) {
        Elements a = group(pair * 2, colW), b = group(pair * 2 + 1, colW);
        if (pair) lines.push_back(text(""));
        for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i)
          lines.push_back(hbox({(i < a.size() ? a[i] : text("")) | size(WIDTH, EQUAL, colW), text("  "),
                                (i < b.size() ? b[i] : text("")) | size(WIDTH, EQUAL, colW)}));
      }
    } else {
      // One column: scroll to keep the focused row in view.
      std::vector<int> rowLine(kModulatorRows, 0);
      for (int g = 0; g < kLfos + kModEnvs; ++g) {
        if (g) lines.push_back(text(""));
        Elements part = group(g, colW);
        const int first = g < kLfos ? g * kLfoParamCount : kLfos * kLfoParamCount + (g - kLfos) * kModEnvParamCount;
        for (std::size_t i = 0; i < part.size(); ++i) {
          if (i > 0) rowLine[static_cast<std::size_t>(first + static_cast<int>(i) - 1)] = static_cast<int>(lines.size());
          lines.push_back(part[i]);
        }
      }
      const int room = std::max(4, ctx.height - 3 - 2 - 2);
      if (static_cast<int>(lines.size()) > room) {
        const int at = rowLine[static_cast<std::size_t>(row_)];
        const int first = std::clamp(at - room / 2, 0, static_cast<int>(lines.size()) - room);
        lines = Elements(lines.begin() + first, lines.begin() + first + room);
      }
    }
    lines.push_back(filler());
    lines.push_back(text(fit("LFOs run all the time; envelopes fire on the track's notes", inner)) | color(palette::faint));
    const std::string title = "Track " + std::to_string(t + 1) + " · modulators" + (ctx.wide() ? "" : " · Tab ⇄");
    return pane(title, vbox(lines), ctx.piano ? palette::value : focused ? palette::accent : palette::border);
  }

  // ---- right: the routes (or the target picker) ----

  Element routes(Context& ctx, int width) {
    if (picking_) return picker(ctx, width);
    const Session& s = ctx.session;
    const int t = ctx.track;
    const bool focused = pane_ == 1;
    const int inner = std::max(24, width - 4);
    const ModuleStatus& st = ctx.module.status();
    const int srcW = 11, depthW = 6, nowW = std::clamp(inner / 6, 6, 12);
    const int targetW = std::max(8, inner - 3 - srcW - 3 - depthW - nowW - 3);
    Elements rows = {hbox({text(" #  "), text("source") | size(WIDTH, EQUAL, srcW), text("   "), text("target") | size(WIDTH, EQUAL, targetW),
                           text(" depth") | size(WIDTH, EQUAL, depthW + 1), text("  now")}) |
                     color(palette::dim)};
    for (int r = 0; r < kModRoutes; ++r) {
      const bool on = focused && r == route_;
      const auto target = s.modTarget(t, r);
      const int source = static_cast<int>(std::lround(s.param(routeRef(t, r, kRouteSource))));
      const float depth = s.param(routeRef(t, r, kRouteDepth));
      std::string targetName = "—";
      if (target) {
        targetName = s.paramLabel(*target);
        const std::string prefix = scopeLabel(t) + " · ";
        if (targetName.rfind(prefix, 0) == 0) targetName = targetName.substr(prefix.size());
      }
      const bool active = target && source != kSrcNone && depth != 0;
      const float live = st.modLive[static_cast<std::size_t>(t)][static_cast<std::size_t>(r)].load(std::memory_order_relaxed);
      char d[16];
      std::snprintf(d, sizeof d, "%+.0f%%", static_cast<double>(depth * 100));
      const Color fg = on ? palette::text : active ? palette::text : palette::faint;
      auto pick = [this, r](Context&) {
        pane_ = 1;
        route_ = r;
      };
      Element row = hbox({
          text(" " + std::to_string(r + 1) + "  ") | color(on ? palette::accent : palette::dim),
          ctx.button(text(fit(modSourceName(source), srcW)) | size(WIDTH, EQUAL, srcW) | color(source == kSrcNone ? palette::faint : palette::lilac),
                     Event::Character(']'), pick),
          text(" → ") | color(palette::faint),
          ctx.button(text(fit(targetName, targetW)) | size(WIDTH, EQUAL, targetW) | color(target ? fg : palette::faint), Event::Return, pick),
          text(" "),
          text(d) | size(WIDTH, EQUAL, depthW) | color(depth == 0 ? palette::faint : fg),
          text("  "),
          live >= 0 ? valueBar(live, nowW, palette::lilac) : text(std::string(static_cast<std::size_t>(nowW), ' ')),
          filler(),
      });
      rows.push_back(ctx.value(on ? row | bgcolor(palette::raised) : row, pick, std::nullopt));
    }
    rows.push_back(text(""));
    rows.push_back(paragraph("v on any value, on any screen, sends LFO 1 to it at 25% on the first free route. Depth is in the "
                             "value's own range; routes onto one value add up. The value you set stays what's saved.") |
                   color(palette::faint));
    rows.push_back(filler());
    return pane("Track " + std::to_string(t + 1) + " · routes" + (ctx.wide() ? "" : " · Tab ⇄"), vbox(rows),
                focused ? palette::accent : palette::border);
  }

  // Every value on the track a route can move, filtered by what's typed.
  std::vector<ParamRef> candidates(const Session& s, int t) const {
    std::vector<ParamRef> out;
    for (int i = 0; i < kModBase; ++i) {
      const ParamRef ref{static_cast<uint8_t>(t), static_cast<uint16_t>(i)};
      if (!s.modulatable(ref)) continue;
      if (!filter_.empty()) {
        std::string label = s.paramLabel(ref), f = filter_;
        for (char& c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (label.find(f) == std::string::npos) continue;
      }
      out.push_back(ref);
    }
    return out;
  }

  void openPicker(Context& ctx) {
    picking_ = true;
    filter_.clear();
    pick_ = 0;
    const auto list = candidates(ctx.session, ctx.track);
    if (const auto current = ctx.session.modTarget(ctx.track, route_))
      for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i] == *current) pick_ = static_cast<int>(i);
  }

  bool pickerEvent(Context& ctx, const Event& e) {
    const auto list = candidates(ctx.session, ctx.track);
    const int n = static_cast<int>(list.size());
    if (e == Event::Escape) { picking_ = false; return true; }
    if (e == Event::ArrowUp) { pick_ = std::max(0, pick_ - 1); return true; }
    if (e == Event::ArrowDown) { pick_ = std::min(std::max(0, n - 1), pick_ + 1); return true; }
    if (e == Event::PageUp) { pick_ = std::max(0, pick_ - 10); return true; }
    if (e == Event::PageDown) { pick_ = std::min(std::max(0, n - 1), pick_ + 10); return true; }
    if (e == Event::Return) {
      if (n > 0) {
        Session& s = ctx.session;
        const ParamRef target = list[static_cast<std::size_t>(std::clamp(pick_, 0, n - 1))];
        s.setModTarget(ctx.track, route_, target);
        // A route with nothing to read, or no depth, would do nothing: start it audibly.
        if (s.param(routeRef(ctx.track, route_, kRouteSource)) == static_cast<float>(kSrcNone))
          s.setParam(routeRef(ctx.track, route_, kRouteSource), static_cast<float>(kSrcLfo1));
        if (s.param(routeRef(ctx.track, route_, kRouteDepth)) == 0.0f) s.setParam(routeRef(ctx.track, route_, kRouteDepth), 0.25f);
      }
      picking_ = false;
      return true;
    }
    if (e == Event::Backspace) {
      if (!filter_.empty()) filter_.pop_back();
      pick_ = 0;
      return true;
    }
    if (e.is_character() && filter_.size() < 24) {
      filter_ += e.character();
      pick_ = 0;
      return true;
    }
    return true;
  }

  Element picker(Context& ctx, int width) {
    const Session& s = ctx.session;
    const int t = ctx.track;
    const auto list = candidates(s, t);
    const int n = static_cast<int>(list.size());
    pick_ = std::clamp(pick_, 0, std::max(0, n - 1));
    const int inner = std::max(20, width - 4);
    Elements rows = {hbox({text("filter ") | color(palette::dim), text(filter_), text(" ") | bgcolor(palette::accent)})};
    const int visible = std::max(3, ctx.height - 3 - 2 - 3);
    const int first = std::clamp(pick_ - visible / 2, 0, std::max(0, n - visible));
    const std::string prefix = scopeLabel(t) + " · ";
    for (int i = first; i < std::min(n, first + visible); ++i) {
      std::string label = s.paramLabel(list[static_cast<std::size_t>(i)]);
      if (label.rfind(prefix, 0) == 0) label = label.substr(prefix.size());
      const bool on = i == pick_;
      Element r = text(" " + fit(label, inner - 2) + " ");
      if (on) r = r | color(palette::bg) | bgcolor(palette::accent);
      rows.push_back(ctx.item(r, [this, i](Context&) { pick_ = i; }));
    }
    if (n == 0) rows.push_back(text(" nothing matches") | color(palette::faint));
    rows.push_back(filler());
    return pane("Route " + std::to_string(route_ + 1) + " → which value?", vbox(rows), palette::accent);
  }

  int pane_ = 1;
  int row_ = 0;
  int route_ = 0;
  bool picking_ = false;
  int pick_ = 0;
  std::string filter_;
};

}  // namespace

float modulatedAt(Context& ctx, ParamRef ref) {
  if (ref.scope >= kTrackCount) return -1;
  for (int r = 0; r < kModRoutes; ++r)
    if (ctx.session.modTarget(ref.scope, r) == ref) {
      const float v = ctx.module.status().modLive[ref.scope][static_cast<std::size_t>(r)].load(std::memory_order_relaxed);
      if (v >= 0) return v;
    }
  return -1;
}

std::unique_ptr<Screen> makeModScreen() { return std::make_unique<ModScreen>(); }

}  // namespace ion::ui
