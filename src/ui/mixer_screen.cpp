// Mixer + EQ: channel strips and master, and the EQ of the selected channel.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <ftxui/dom/canvas.hpp>

#include "engine/channel_eq.h"
#include "engine/module.h"
#include "project/session.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;

constexpr int kMasterIndex = kTrackCount;  // strip 8

std::vector<float> trackValues(const Session& s, int t) {
  std::vector<float> v(kMixerParamCount);
  for (int i = 0; i < kMixerParamCount; ++i) v[static_cast<std::size_t>(i)] = s.param({static_cast<uint8_t>(t), static_cast<uint8_t>(i)});
  return v;
}

// Six cells of the EQ response, sampled across the spectrum.
std::string eqGlyph(const Session& s, int t) {
  static const char* levels[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  const auto v = trackValues(s, t);
  const EqSettings eq = EqSettings::fromParams(v.data());
  std::string out;
  for (double f : {40.0, 150.0, 500.0, 1500.0, 5000.0, 14000.0}) {
    const double db = eqResponseDb(eq, f, kSampleRate);
    const int level = static_cast<int>(std::lround(std::clamp((db + 12.0) / 24.0, 0.0, 1.0) * 7.0));
    out += levels[level];
  }
  return out;
}

struct Band {
  const char* name;
  int on, freq, gain, q;  // MixerParam indices, -1 = none
};
const std::array<Band, 6> kBands = {{
    {"high-pass", kHpOn, kHpFreq, kHpSlope, -1},
    {"low shelf", kLsOn, kLsFreq, kLsGain, kLsQ},
    {"bell 1", kB1On, kB1Freq, kB1Gain, kB1Q},
    {"bell 2", kB2On, kB2Freq, kB2Gain, kB2Q},
    {"high shelf", kHsOn, kHsFreq, kHsGain, kHsQ},
    {"low-pass", kLpOn, kLpFreq, -1, -1},
}};

int fieldParam(const Band& b, int field) {
  switch (field) {
    case 0: return b.on;
    case 1: return b.freq;
    case 2: return b.gain;
    default: return b.q;
  }
}

class MixerScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    if (strip_ < kMasterIndex) strip_ = ctx.track;
    const int eqTrack = strip_ == kMasterIndex ? ctx.track : strip_;
    Element strips = ctx.narrow() ? stripList(ctx) : stripRow(ctx);
    Element eq = eqPane(ctx, eqTrack);
    return vbox({strips, eq | flex});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = (pane_ == 0 && strip_ != kMasterIndex) ? 1 : 0;
      return true;
    }
    if (e == Event::Character('b')) {
      const int t = strip_ == kMasterIndex ? ctx.track : strip_;
      s.nudgeParam({static_cast<uint8_t>(t), kEqBypass}, 1, false);
      return true;
    }
    const auto scope = static_cast<uint8_t>(strip_);
    if (pane_ == 0) {
      const bool narrowList = ctx.narrow();
      // In the wide strip row ←→ picks a channel and ↑↓ moves its fader; the
      // narrow list is vertical, so the two swap.
      const Event prev = narrowList ? Event::ArrowUp : Event::ArrowLeft;
      const Event next = narrowList ? Event::ArrowDown : Event::ArrowRight;
      if (e == prev) { strip_ = (strip_ + kMasterIndex) % (kMasterIndex + 1); syncTrack(ctx); return true; }
      if (e == next) { strip_ = (strip_ + 1) % (kMasterIndex + 1); syncTrack(ctx); return true; }
      int dir = 0;
      bool coarse = false;
      if (!narrowList) {
        if (e == Event::ArrowUp) dir = 1;
        else if (e == Event::ArrowDown) dir = -1;
        else if (e == kShiftUp || e == Event::ArrowUpCtrl) { dir = 1; coarse = true; }
        else if (e == kShiftDown || e == Event::ArrowDownCtrl) { dir = -1; coarse = true; }
      } else if (const Nudge n = nudgeOf(e); n.direction) {
        dir = n.direction;
        coarse = n.coarse;
      }
      if (dir) {
        s.nudgeParam({scope, kFader}, dir, coarse);
        return true;
      }
      if (strip_ != kMasterIndex) {
        if (e == Event::Character('[')) { s.nudgeParam({scope, kPan}, -1, false); return true; }
        if (e == Event::Character(']')) { s.nudgeParam({scope, kPan}, 1, false); return true; }
        if (e == Event::Return) { pane_ = 1; return true; }
      }
      return false;
    }
    // EQ pane
    if (e == Event::ArrowUp) { band_ = (band_ + 5) % 6; clampField(); return true; }
    if (e == Event::ArrowDown) { band_ = (band_ + 1) % 6; clampField(); return true; }
    if (e == Event::Return) {
      do field_ = (field_ + 1) % 4; while (fieldParam(kBands[static_cast<std::size_t>(band_)], field_) < 0);
      return true;
    }
    if (const Nudge n = nudgeOf(e); n.direction) {
      const int p = fieldParam(kBands[static_cast<std::size_t>(band_)], field_);
      if (p >= 0) s.nudgeParam({static_cast<uint8_t>(ctx.track), static_cast<uint8_t>(p)}, n.direction, n.coarse);
      return true;
    }
    return false;
  }

  Hints hints(Context& ctx) override {
    if (pane_ == 1)
      return {{"↑↓", "Band"}, {"⏎", "Field"}, {"←→", "Value"}, {"b", "Bypass"}, {"Tab", "Strips"},
              {"l", "Learn"}, {"Esc", "Back"}};
    if (ctx.narrow())
      return {{"↑↓", "Channel"}, {"←→", "Fader"}, {"[ ]", "Pan"}, {"Tab", "EQ"}, {"m s", "Mute/Solo"}, {"Esc", "Back"}};
    return {{"←→", "Channel"}, {"↑↓", "Fader"}, {"[ ]", "Pan"}, {"Tab", "EQ"}, {"b", "Bypass"},
            {"m", "Mute"},     {"s", "Solo"},   {"l", "Learn"}, {"Esc", "Back"}};
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    if (pane_ == 0) return ParamRef{static_cast<uint8_t>(strip_), kFader};
    const int p = fieldParam(kBands[static_cast<std::size_t>(band_)], field_);
    if (p < 0) return std::nullopt;
    return ParamRef{static_cast<uint8_t>(ctx.track), static_cast<uint8_t>(p)};
  }

 private:
  void syncTrack(Context& ctx) {
    if (strip_ < kMasterIndex) ctx.track = strip_;
  }
  void clampField() {
    while (fieldParam(kBands[static_cast<std::size_t>(band_)], field_) < 0) field_ = (field_ + 3) % 4;
  }

  Element strip(Context& ctx, int index, int width, int meterHeight) {
    const Session& s = ctx.session;
    const ModuleStatus& st = ctx.module.status();
    const bool master = index == kMasterIndex;
    const bool sel = index == strip_;
    const auto scope = static_cast<uint8_t>(index);
    const EngineType type = master ? EngineType::None : s.track(index).engine;
    const MeterStatus& m = master ? st.master : st.track[static_cast<std::size_t>(index)];

    const float fader = s.param({scope, kFader});
    const float faderN = toNormalized(mixerParam(kFader), fader);
    const int cap = static_cast<int>(std::lround(faderN * static_cast<float>(meterHeight - 1)));
    Elements faderCol;
    for (int row = meterHeight - 1; row >= 0; --row)
      faderCol.push_back(row == cap ? text("━━") | color(sel ? palette::accent : palette::text) : text(" │") | color(palette::border));

    const std::string id = master ? "M" : std::to_string(index + 1);
    const std::string label = master ? "OUT" : engineLabel(type);
    const std::string name = master ? "master" : (type == EngineType::None ? "empty" : trackSoundName(s, index));
    const bool muted = !master && s.param({scope, kMute}) >= 0.5f;
    const bool solo = !master && s.param({scope, kSolo}) >= 0.5f;
    const bool eqBypassed = !master && s.param({scope, kEqBypass}) >= 0.5f;

    Elements col = {
        hbox({text(id + " ") | bold | color(sel ? palette::accent : palette::text),
              text(label) | color(master ? palette::accent : engineColor(type))}),
        text(fit(name, width - 2)) | color(type == EngineType::None && !master ? palette::faint : palette::text),
        text(master ? " " : eqGlyph(s, index)) | color(eqBypassed || type == EngineType::None ? palette::faint : palette::value),
        text(master ? " " : "pan " + formatValue(mixerParam(kPan), s.param({scope, kPan}))) |
            color(type == EngineType::None ? palette::faint : palette::dim),
        hbox({verticalMeter(holds_[static_cast<std::size_t>(index)][0].update(m.rmsLeft.load(std::memory_order_relaxed),
                                                                              m.peakLeft.load(std::memory_order_relaxed)),
                            meterHeight),
              verticalMeter(holds_[static_cast<std::size_t>(index)][1].update(m.rmsRight.load(std::memory_order_relaxed),
                                                                              m.peakRight.load(std::memory_order_relaxed)),
                            meterHeight),
              vbox(faderCol)}) |
            center,
        text(formatValue(mixerParam(kFader), fader)) | color(sel ? palette::text : palette::dim) | center,
        master ? text(" ")
               : hbox({text(" M ") | color(muted ? palette::bg : palette::faint) | bgcolor(muted ? palette::record : palette::bg),
                       text(" "),
                       text(" S ") | color(solo ? palette::bg : palette::faint) | bgcolor(solo ? palette::accent : palette::bg)}) |
                     center,
    };
    const Color border = sel && pane_ == 0 ? palette::accent : (master ? palette::value : palette::border);
    return vbox(col) | size(WIDTH, EQUAL, width) | borderStyled(ROUNDED, border) | (sel ? bgcolor(palette::raised) : nothing);
  }

  Element stripRow(Context& ctx) {
    const int width = std::max(10, (ctx.width - 2) / (kMasterIndex + 1) - 2);
    const int meterHeight = std::clamp(ctx.height - 26, 4, 10);
    Elements strips;
    for (int i = 0; i <= kMasterIndex; ++i) strips.push_back(strip(ctx, i, width, meterHeight));
    return hbox(strips);
  }

  Element stripList(Context& ctx) {
    const Session& s = ctx.session;
    const ModuleStatus& st = ctx.module.status();
    Elements rows;
    for (int i = 0; i <= kMasterIndex; ++i) {
      const bool master = i == kMasterIndex;
      const auto scope = static_cast<uint8_t>(i);
      const bool sel = i == strip_ && pane_ == 0;
      const MeterStatus& m = master ? st.master : st.track[static_cast<std::size_t>(i)];
      const MeterReading reading = holds_[static_cast<std::size_t>(i)][0].update(std::max(m.rmsLeft.load(), m.rmsRight.load()),
                                                                                 std::max(m.peakLeft.load(), m.peakRight.load()));
      std::string ms = master ? "" : (s.param({scope, kMute}) >= 0.5f ? "M" : (s.param({scope, kSolo}) >= 0.5f ? "S" : ""));
      Element row = hbox({
          text(master ? " M " : " " + std::to_string(i + 1) + " ") | bold,
          text(fit(master ? "master" : engineLabel(s.track(i).engine), 5)) | size(WIDTH, EQUAL, 6) |
              color(sel ? palette::bg : (master ? palette::accent : engineColor(s.track(i).engine))),
          levelBar(reading, 6, sel),
          text(" " + formatValue(mixerParam(kFader), s.param({scope, kFader}))) | size(WIDTH, EQUAL, 10),
          text(master ? "" : formatValue(mixerParam(kPan), s.param({scope, kPan}))) | size(WIDTH, EQUAL, 5),
          text(eqGlyph(s, std::min(i, kTrackCount - 1))) | color(sel ? palette::bg : palette::value),
          text(" " + ms) | bold,
          filler(),
      });
      rows.push_back(sel ? row | color(palette::bg) | bgcolor(palette::accent) : row);
    }
    return pane("Channels", vbox(rows), pane_ == 0 ? palette::accent : palette::border);
  }

  Element curve(Context& ctx, int t, int cols, int rows) {
    const auto v = trackValues(ctx.session, t);
    const EqSettings eq = EqSettings::fromParams(v.data());
    const int w = std::max(2, cols * 2), h = std::max(4, rows * 4);
    Canvas c(w, h);
    auto yOf = [&](double db) { return static_cast<int>(std::lround((1.0 - (std::clamp(db, -18.0, 18.0) + 18.0) / 36.0) * (h - 1))); };
    for (int x = 0; x < w; x += 2) c.DrawPoint(x, yOf(0), true, palette::borderSubtle);
    for (double f : {100.0, 1000.0, 10000.0}) {
      const int x = static_cast<int>(std::lround(std::log(f / 20.0) / std::log(1000.0) * (w - 1)));
      for (int y = 0; y < h; y += 3) c.DrawPoint(x, y, true, palette::borderSubtle);
    }
    int px = 0, py = yOf(eqResponseDb(eq, 20.0, kSampleRate));
    const Color line = eq.bypass ? palette::faint : palette::value;
    for (int x = 1; x < w; ++x) {
      const double f = 20.0 * std::pow(1000.0, static_cast<double>(x) / (w - 1));
      const int y = yOf(eqResponseDb(eq, f, kSampleRate));
      c.DrawPointLine(px, py, x, y, line);
      px = x;
      py = y;
    }
    return canvas(std::move(c));
  }

  Element bandTable(Context& ctx, int t) {
    const Session& s = ctx.session;
    const auto tt = static_cast<uint8_t>(t);
    Elements rows;
    rows.push_back(hbox({text("band        ") , text("on   "), text("freq      "), text("gain     "), text("Q")}) | color(palette::dim));
    for (int b = 0; b < 6; ++b) {
      const Band& band = kBands[static_cast<std::size_t>(b)];
      Elements cells = {text(std::string(band.name) + std::string(12 - std::string(band.name).size(), ' '))};
      const int widths[4] = {5, 10, 9, 6};
      for (int f = 0; f < 4; ++f) {
        const int p = fieldParam(band, f);
        std::string value = p < 0 ? "·" : formatValue(mixerParam(static_cast<MixerParam>(p)), s.param({tt, static_cast<uint8_t>(p)}));
        if (f == 0 && p >= 0) value = s.param({tt, static_cast<uint8_t>(p)}) >= 0.5f ? "●" : "○";
        const bool focused = pane_ == 1 && b == band_ && f == field_;
        Element cell = text(value) | size(WIDTH, EQUAL, widths[f]);
        if (focused) cell = cell | color(palette::bg) | bgcolor(palette::accent);
        else if (f == 0) cell = cell | color(value == "●" ? palette::value : palette::faint);
        else cell = cell | color(palette::text);
        cells.push_back(cell);
      }
      rows.push_back(hbox(cells));
    }
    rows.push_back(text(""));
    const bool bypass = s.param({tt, kEqBypass}) >= 0.5f;
    rows.push_back(hbox({text("eq "), text(bypass ? "bypassed" : "● on") | color(bypass ? palette::record : palette::value),
                         text("   b compares") | color(palette::dim)}));
    rows.push_back(text("eq sits before inserts") | color(palette::faint));
    return vbox(rows);
  }

  Element eqPane(Context& ctx, int t) {
    const Session& s = ctx.session;
    const TrackState& tr = s.track(t);
    std::string title = "EQ · track " + std::to_string(t + 1);
    if (tr.engine != EngineType::None) title += " · " + trackSoundName(s, t);
    const int tableWidth = 44;
    const int curveCols = ctx.narrow() ? std::max(10, ctx.width - 4) : std::max(10, ctx.width - tableWidth - 8);
    const int curveRows = std::clamp(ctx.height - (ctx.narrow() ? 24 : 27), 3, 12);
    // Labels placed at the same log-frequency positions as the gridlines.
    std::string axis(static_cast<std::size_t>(curveCols), ' ');
    auto place = [&](double f, const char* label, bool right = false) {
      const int x = static_cast<int>(std::lround(std::log(f / 20.0) / std::log(1000.0) * (curveCols - 1)));
      const int len = static_cast<int>(std::strlen(label));
      const int at = std::clamp(right ? x - len + 1 : x - len / 2, 0, curveCols - len);
      // Skip a label that would touch one already placed.
      for (int i = std::max(0, at - 1); i < std::min(curveCols, at + len + 1); ++i)
        if (axis[static_cast<std::size_t>(i)] != ' ') return;
      axis.replace(static_cast<std::size_t>(at), static_cast<std::size_t>(len), label);
    };
    place(20, "20");
    place(20000, "20k", true);
    place(1000, "1k");
    place(100, "100");
    place(10000, "10k");
    Element curvePart = vbox({curve(ctx, t, curveCols, curveRows), text(axis) | color(palette::faint),
                              text("±18 dB") | color(palette::faint)});
    Element body = ctx.narrow() ? vbox({curvePart, separatorLight() | color(palette::borderSubtle), bandTable(ctx, t)})
                                : hbox({curvePart | flex, text("  "), bandTable(ctx, t) | size(WIDTH, EQUAL, tableWidth)});
    return pane(title, body, pane_ == 1 ? palette::accent : palette::border);
  }

  std::array<std::array<PeakHold, 2>, kMasterIndex + 1> holds_{};  // per strip, L and R
  int strip_ = 0;  // 0-7 tracks, 8 master
  int pane_ = 0;   // 0 strips, 1 EQ
  int band_ = 2;
  int field_ = 2;
};

}  // namespace

std::unique_ptr<Screen> makeMixerScreen() { return std::make_unique<MixerScreen>(); }

}  // namespace ion::ui
