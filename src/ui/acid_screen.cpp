// The Instrument screen for ACID tracks (DECISIONS D-60): the 303's knobs in
// its own order, then how it plays, then the amp envelope drawn and placed
// as on FM and Plaits.

#include <cmath>
#include <cstdio>

#include "engine/acid/acid_engine.h"
#include "engine/envelope_params.h"
#include "engine/module.h"
#include "project/session.h"
#include "ui/envelope_view.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;
using A = AcidEngine;

// ↑↓ order: the front panel, then the amp envelope (as on FM: straight after
// the knobs), then playing.
const int kRows[] = {A::kWaveform,  A::kCutoff,  A::kResonance, A::kEnvMod,     A::kDecay,      A::kAccent,
                     A::kAmpAttack, A::kAmpHold, A::kAmpDecay,  A::kAmpSustain, A::kAmpRelease, A::kVelAmp,
                     A::kSlide,     A::kAccentAt, A::kTune,     A::kFine,       A::kBendRange,  A::kLevel};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);
constexpr int kPanelRows = 6, kAmpRows = 6;
const char* const kRoles[] = {"saw ↔ square", "where the filter sits", "the squelch", "how far each note opens it",
                              "how fast it closes", "how much harder accents hit"};

class AcidScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const Session& s = ctx.session;
    const int t = ctx.track;
    const int inner = std::max(24, ctx.width - 4);
    const bool twoColumns = inner >= 56;
    const int colW = twoColumns ? (inner - 2) / 2 : inner;
    const auto table = A::paramTable();

    auto cell = [&](int r, int w, bool bar) {
      const int param = kRows[r];
      const ParamInfo& pi = table[static_cast<std::size_t>(param)];
      const float v = s.param(engineParamRef(t, param));
      const bool on = r == row_;
      Elements cells = {text(on ? "▸ " : "  ") | color(palette::accent),
                        text(fit(pi.label, 11)) | size(WIDTH, EQUAL, 11) | color(on ? palette::accent : palette::text)};
      if (bar) {
        const int barW = std::clamp(w - 40, 8, 16);
        cells.push_back(valueBar(toNormalized(pi, v), barW, on ? palette::accent : palette::acid, false,
                                 modulatedAt(ctx, engineParamRef(t, param))));
        cells.push_back(text(" " + fit(formatValue(pi, v), 9)) | size(WIDTH, EQUAL, 10) | color(on ? palette::text : palette::value));
        cells.push_back(text(fit(kRoles[r], std::max(0, w - 13 - barW - 11))) | color(palette::dim));
      } else {
        cells.push_back(text(" " + formatValue(pi, v)) | color(on ? palette::text : palette::dim));
      }
      cells.push_back(filler());
      Element e = hbox(cells);
      return ctx.value(on ? e | bgcolor(palette::raised) : e, [this, r](Context&) { row_ = r; });
    };

    Elements lines;
    for (int r = 0; r < kPanelRows; ++r) lines.push_back(cell(r, inner, true));
    lines.push_back(text(fit("overlapping notes slide · velocity " +
                                 std::to_string(static_cast<int>(std::lround(s.param(engineParamRef(t, A::kAccentAt))))) +
                                 " and up is an accent",
                             inner)) |
                    color(palette::faint));
    // Playing beside the amp envelope, as FM sets its envelope beside its macros.
    Elements play = {text("play") | color(palette::dim)};
    for (int r = kPanelRows + kAmpRows; r < kRowCount; ++r) play.push_back(cell(r, colW, false));
    std::array<float, kEnvParamCount> env{};
    for (int k = 0; k < kEnvParamCount; ++k) env[static_cast<std::size_t>(k)] = s.param(engineParamRef(t, A::kAmpAttack + k));
    Elements amp = {hbox({text("amp ") | color(palette::dim), text(envelopeShape(env.data(), std::max(6, colW - 6))) | color(palette::value)})};
    for (int r = kPanelRows; r < kPanelRows + kAmpRows; ++r) amp.push_back(cell(r, colW, false));
    lines.push_back(text(""));
    if (twoColumns) {
      for (std::size_t i = 0; i < std::max(play.size(), amp.size()); ++i)
        lines.push_back(hbox({(i < amp.size() ? amp[i] : text("")) | size(WIDTH, EQUAL, colW), text("  "),
                              (i < play.size() ? play[i] : text("")) | size(WIDTH, EQUAL, colW)}));
    } else {
      for (Element& e : amp) lines.push_back(e);
      lines.push_back(text(""));
      for (Element& e : play) lines.push_back(e);
    }
    // Scroll to the focused row when the screen is short.
    const int room = std::max(4, ctx.height - 3 - 2 - 2);
    if (static_cast<int>(lines.size()) > room) {
      const int at = row_ < kPanelRows ? row_ : static_cast<int>(lines.size()) - 1;
      const int first = std::clamp(at - room / 2, 0, static_cast<int>(lines.size()) - room);
      lines = Elements(lines.begin() + first, lines.begin() + first + room);
    }
    lines.push_back(filler());
    // CCs on this track's ACID.
    std::string ccs;
    for (const Session::CcEntry& c : s.ccMappings()) {
      if (c.mapping.target.scope != t || !c.mapping.target.isEngine()) continue;
      const int p = c.mapping.target.index - kEngineParamBase;
      if (p < 0 || p >= A::kParamCount) continue;
      std::string label = table[static_cast<std::size_t>(p)].label;
      for (char& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      ccs += (ccs.empty() ? "" : " · ") + ("cc " + std::to_string(c.mapping.cc) + " → " + label);
    }
    lines.push_back(separatorLight() | color(palette::borderSubtle));
    lines.push_back(text(fit(ccs.empty() ? "from the OXI: l on a value learns a CC · v modulates it" : "from the OXI: " + ccs, inner)) |
                    color(ccs.empty() ? palette::faint : palette::value));
    return pane("Track " + std::to_string(t + 1) + " · ACID · 303-style bass", vbox(lines), ctx.piano ? palette::value : palette::accent) | flex;
  }

  bool onEvent(Context& ctx, const Event& e) override {
    if (e == Event::ArrowUp) { row_ = (row_ + kRowCount - 1) % kRowCount; return true; }
    if (e == Event::ArrowDown) { row_ = (row_ + 1) % kRowCount; return true; }
    if (e == Event::Return) { ctx.request = Context::Request::Audition; return true; }
    if (const Nudge n = nudgeOf(e); n.direction) {
      ctx.session.nudgeParam(engineParamRef(ctx.track, kRows[row_]), n.direction, n.coarse);
      return true;
    }
    return false;
  }

  Hints hints(Context&) override {
    return {{"↑↓", "Knob"}, {"←→", "Turn"}, {"l", "Learn"}, {"v", "Modulate"}, {"⏎", "Play"}, {"Esc", "Back"}, {"?", "Help"}};
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override { return engineParamRef(ctx.track, kRows[row_]); }

 private:
  int row_ = 1;  // cutoff: the one you reach for
};

}  // namespace

std::unique_ptr<Screen> makeAcidScreen() { return std::make_unique<AcidScreen>(); }

}  // namespace ion::ui
