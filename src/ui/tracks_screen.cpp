// Tracks (home): the track list, and a detail pane for the selected track.

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "engine/module.h"
#include "project/session.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;
using Clock = std::chrono::steady_clock;

std::string soundName(const Session& s, int t) {
  const TrackState& tr = s.track(t);
  switch (tr.engine) {
    case EngineType::None: return "— empty —";
    case EngineType::Fm: return tr.hasVoice ? syx::name(tr.voice) : "INIT VOICE";
    default: return engineLabel(tr.engine);
  }
}

// "hp 40 · lo +2 · 420 −1.5 · 2.5k +1.5"
std::string eqSummary(const Session& s, int t) {
  auto p = [&](MixerParam m) { return s.param({static_cast<uint8_t>(t), m}); };
  if (p(kEqBypass) >= 0.5f) return "bypassed";
  auto freq = [](float f) {
    char b[16];
    if (f >= 1000) std::snprintf(b, sizeof b, "%.1fk", static_cast<double>(f / 1000));
    else std::snprintf(b, sizeof b, "%.0f", static_cast<double>(f));
    return std::string(b);
  };
  auto gain = [](float g) {
    char b[16];
    std::snprintf(b, sizeof b, "%+.1f", static_cast<double>(g));
    return std::string(b);
  };
  std::string out;
  auto add = [&](const std::string& part) { out += (out.empty() ? "" : " · ") + part; };
  if (p(kHpOn) >= 0.5f) add("hp " + freq(p(kHpFreq)));
  if (p(kLsOn) >= 0.5f && p(kLsGain) != 0) add("lo " + gain(p(kLsGain)));
  if (p(kB1On) >= 0.5f && p(kB1Gain) != 0) add(freq(p(kB1Freq)) + " " + gain(p(kB1Gain)));
  if (p(kB2On) >= 0.5f && p(kB2Gain) != 0) add(freq(p(kB2Freq)) + " " + gain(p(kB2Gain)));
  if (p(kHsOn) >= 0.5f && p(kHsGain) != 0) add("hi " + gain(p(kHsGain)));
  if (p(kLpOn) >= 0.5f) add("lp " + freq(p(kLpFreq)));
  return out.empty() ? "flat" : out;
}

class TracksScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const Session& s = ctx.session;
    const ModuleStatus& st = ctx.module.status();
    const auto now = Clock::now();

    // ---- track list ----
    Elements rows;
    const bool listFocused = pane_ == 0;
    const int meterWidth = ctx.narrow() ? 6 : 10;
    rows.push_back(hbox({text(" #  ch  engine  "), text("sound") | flex,
                         text(ctx.narrow() ? "level  " : "level       dB    "), text("    ")}) |
                   color(palette::dim));
    int used = 0;
    for (int t = 0; t < kTrackCount; ++t) {
      const TrackState& tr = s.track(t);
      if (tr.engine != EngineType::None) ++used;
      const uint32_t notes = st.trackNotes[static_cast<std::size_t>(t)].load(std::memory_order_relaxed);
      if (notes != lastNotes_[static_cast<std::size_t>(t)]) {
        lastNotes_[static_cast<std::size_t>(t)] = notes;
        lastActivity_[static_cast<std::size_t>(t)] = now;
      }
      const bool active = now - lastActivity_[static_cast<std::size_t>(t)] < std::chrono::milliseconds(150);
      const bool sel = t == ctx.track;
      const bool inverse = sel && listFocused;
      const bool empty = tr.engine == EngineType::None;
      const Color fg = inverse ? palette::bg : (empty ? palette::faint : palette::text);
      const Color dimFg = inverse ? palette::bg : palette::dim;

      const auto& m = st.track[static_cast<std::size_t>(t)];
      const float peak = std::max(m.peakLeft.load(std::memory_order_relaxed), m.peakRight.load(std::memory_order_relaxed));
      char head[16];
      std::snprintf(head, sizeof head, " %d  ", t + 1);
      char ch[16];
      std::snprintf(ch, sizeof ch, "%02d  ", tr.channel + 1);
      const float fader = s.param({static_cast<uint8_t>(t), kFader});
      std::string ms;
      if (s.param({static_cast<uint8_t>(t), kSolo}) >= 0.5f) ms = "S";
      if (s.param({static_cast<uint8_t>(t), kMute}) >= 0.5f) ms = "M";
      char db[16];
      if (fader <= kFaderMinDb) std::snprintf(db, sizeof db, "   −∞ ");
      else std::snprintf(db, sizeof db, "%5.1f ", static_cast<double>(fader));

      Elements cells = {
          text(head) | bold | color(fg),
          text(ch) | color(dimFg),
          text(fit(engineLabel(tr.engine), 6)) | size(WIDTH, EQUAL, 8) | bold |
              color(inverse ? palette::bg : engineColor(tr.engine)),
          text(soundName(s, t)) | color(fg) | flex,
          text(" "),
          levelBar(peak, meterWidth, inverse),
      };
      if (!ctx.narrow()) cells.push_back(text(empty ? "       " : " " + std::string(db)) | color(dimFg));
      cells.push_back(text(active ? " ● " : " · ") | color(inverse ? palette::bg : (active ? palette::value : palette::faint)));
      cells.push_back(text(ms.empty() ? " " : ms) | bold | color(ms == "M" ? (inverse ? palette::bg : palette::record) : fg));
      Element row = hbox(cells);
      if (inverse) row = row | bgcolor(palette::accent);
      else if (sel) row = row | bgcolor(palette::raised);
      rows.push_back(row);
    }
    rows.push_back(filler());
    rows.push_back(separatorLight() | color(palette::borderSubtle));
    char footer[96];
    std::snprintf(footer, sizeof footer, " %d of 8 tracks in use · %d voices", used, st.voices.load());
    rows.push_back(text(footer) | color(palette::dim));
    const Color listBorder = ctx.piano ? palette::value : (listFocused ? palette::accent : palette::border);
    Element list = pane("Tracks", vbox(rows), listBorder);

    if (ctx.narrow()) return pane_ == 0 ? list | flex : detail(ctx) | flex;
    return hbox({list | flex, detail(ctx) | size(WIDTH, EQUAL, std::max(36, ctx.width * 38 / 100))});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = 1 - pane_;
      row_ = 0;
      return true;
    }
    if (pane_ == 0) {
      if (e == Event::ArrowUp) { ctx.track = (ctx.track + kTrackCount - 1) % kTrackCount; return true; }
      if (e == Event::ArrowDown) { ctx.track = (ctx.track + 1) % kTrackCount; return true; }
      if (const Nudge n = nudgeOf(e); n.direction) {
        s.nudgeParam({static_cast<uint8_t>(ctx.track), kFader}, n.direction, n.coarse);
        return true;
      }
      if (e == Event::Return) {
        ctx.request = s.track(ctx.track).engine == EngineType::None ? Context::Request::ChooseEngine
                                                                      : Context::Request::ShowInstrument;
        return true;
      }
    } else {
      const auto rows = detailRows(ctx);
      if (e == Event::ArrowUp) { row_ = (row_ + static_cast<int>(rows.size()) - 1) % static_cast<int>(rows.size()); return true; }
      if (e == Event::ArrowDown) { row_ = (row_ + 1) % static_cast<int>(rows.size()); return true; }
      row_ = std::min(row_, static_cast<int>(rows.size()) - 1);
      const Row& r = rows[static_cast<std::size_t>(row_)];
      if (const Nudge n = nudgeOf(e); n.direction) {
        if (r.param) s.nudgeParam(*r.param, n.direction, n.coarse);
        else if (r.kind == Row::Patch) s.stepBank(ctx.track, n.direction * (n.coarse ? 8 : 1));
        else if (r.kind == Row::Channel) s.setChannel(ctx.track, s.track(ctx.track).channel + n.direction);
        return true;
      }
      if (e == Event::Return) {
        if (r.kind == Row::Patch) ctx.request = Context::Request::ShowInstrument;
        else if (r.kind == Row::Eq) ctx.request = Context::Request::ShowMixer;
        else if (r.kind == Row::Engine) ctx.request = Context::Request::ChooseEngine;
        return true;
      }
    }
    if (e == Event::Character('e')) {
      ctx.request = Context::Request::ChooseEngine;
      return true;
    }
    return false;
  }

  Hints hints(Context& ctx) override {
    if (pane_ == 1)
      return {{"↑↓", "Row"}, {"←→", "Change"}, {"Tab", "List"}, {"⏎", "Open"}, {"Spc", "Audition"},
              {"l", "Learn"}, {"?", "Help"}, {"q", "Quit"}};
    const bool empty = ctx.session.track(ctx.track).engine == EngineType::None;
    return {{"1-8", "Track"}, {"↑↓", "Move"}, {"←→", "Fader"}, {"Tab", "Pane"}, {"⏎", empty ? "Engine" : "Edit"},
            {"m", "Mute"},    {"s", "Solo"},  {"Spc", "Audition"}, {"?", "Help"}, {"q", "Quit"}};
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    if (pane_ == 0) return ParamRef{static_cast<uint8_t>(ctx.track), kFader};
    const auto rows = detailRows(ctx);
    return rows[static_cast<std::size_t>(std::min(row_, static_cast<int>(rows.size()) - 1))].param;
  }

 private:
  struct Row {
    enum Kind { Param, Patch, Channel, Eq, Engine } kind;
    std::string label;
    std::optional<ParamRef> param;
  };

  std::vector<Row> detailRows(Context& ctx) const {
    const int t = ctx.track;
    const auto tt = static_cast<uint8_t>(t);
    std::vector<Row> rows;
    const EngineType type = ctx.session.track(t).engine;
    rows.push_back({Row::Engine, "engine", std::nullopt});
    if (type == EngineType::Fm) rows.push_back({Row::Patch, "patch", std::nullopt});
    const auto table = engineParams(type);
    for (std::size_t i = 0; i < table.size(); ++i) {
      std::string label = table[i].label;
      if (!label.empty()) label[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(label[0])));
      rows.push_back({Row::Param, label, ParamRef{tt, static_cast<uint8_t>(kEngineParamBase + i)}});
    }
    rows.push_back({Row::Param, "fader", ParamRef{tt, kFader}});
    rows.push_back({Row::Param, "pan", ParamRef{tt, kPan}});
    rows.push_back({Row::Eq, "eq", std::nullopt});
    rows.push_back({Row::Channel, "midi ch", std::nullopt});
    return rows;
  }

  Element detail(Context& ctx) {
    const Session& s = ctx.session;
    const int t = ctx.track;
    const TrackState& tr = s.track(t);
    const bool focused = pane_ == 1;
    const auto rows = detailRows(ctx);
    row_ = std::min(row_, static_cast<int>(rows.size()) - 1);

    Elements out;
    out.push_back(text(soundName(s, t)) | bold | color(tr.engine == EngineType::None ? palette::faint : engineColor(tr.engine)));
    if (tr.engine == EngineType::Fm) {
      std::string from = tr.hasVoice ? "from " + (tr.voiceSource.empty() ? std::string("project") : tr.voiceSource) : "Dexed init voice";
      if (tr.bankIndex >= 0 && !s.bank().empty())
        from += " · voice " + std::to_string(tr.bankIndex + 1) + " of " + std::to_string(s.bank().size());
      out.push_back(text(from) | color(palette::dim));
    } else if (tr.engine == EngineType::None) {
      out.push_back(text("⏎ on the track list chooses an engine") | color(palette::dim));
    }
    out.push_back(text(""));

    for (std::size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      const bool f = focused && static_cast<int>(i) == row_;
      std::string value;
      Element bar = nullptr;
      switch (r.kind) {
        case Row::Engine: value = tr.engine == EngineType::None ? "empty" : engineLabel(tr.engine); break;
        case Row::Patch: value = s.bank().empty() ? "no bank (--syx)" : "◀ " + soundName(s, t) + " ▶"; break;
        case Row::Channel: value = std::to_string(tr.channel + 1); break;
        case Row::Eq: value = fit(eqSummary(s, t), 24); break;
        case Row::Param: {
          const ParamInfo* pi = s.paramInfo(*r.param);
          const float v = s.param(*r.param);
          value = pi ? formatValue(*pi, v) : "?";
          if (pi && (pi->curve == Curve::Fader || pi->curve == Curve::Linear)) bar = valueBar(toNormalized(*pi, v), 10, palette::value);
          break;
        }
      }
      if (r.kind == Row::Param && r.label == "fader") {
        out.push_back(text(""));
        out.push_back(text("mixer") | color(palette::dim));
      }
      out.push_back(paramRow(r.label, value, f, 10, bar));
    }
    out.push_back(text(""));
    out.push_back(text("inserts · sends   arrive in Phase 4") | color(palette::faint));
    out.push_back(filler());
    out.push_back(separatorLight() | color(palette::borderSubtle));
    char foot[64];
    std::snprintf(foot, sizeof foot, "midi ch %02d · %d voices", tr.channel + 1,
                  ctx.module.status().trackVoices[static_cast<std::size_t>(t)].load());
    out.push_back(text(foot) | color(palette::dim));

    std::string title = "Track " + std::to_string(t + 1);
    if (tr.engine != EngineType::None) title += std::string(" · ") + engineLabel(tr.engine);
    const Color border = ctx.piano ? palette::value : (focused ? palette::accent : palette::border);
    return pane(title, vbox(out), border);
  }

  int pane_ = 0;
  int row_ = 0;
  std::array<uint32_t, kTrackCount> lastNotes_{};
  std::array<Clock::time_point, kTrackCount> lastActivity_{};
};

}  // namespace

std::unique_ptr<Screen> makeTracksScreen() { return std::make_unique<TracksScreen>(); }

Nudge nudgeOf(const Event& e) {
  if (e == Event::ArrowLeft) return {-1, false};
  if (e == Event::ArrowRight) return {1, false};
  if (e == kShiftLeft || e == Event::ArrowLeftCtrl) return {-1, true};
  if (e == kShiftRight || e == Event::ArrowRightCtrl) return {1, true};
  return {};
}

}  // namespace ion::ui
