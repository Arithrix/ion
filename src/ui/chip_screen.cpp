// The Instrument screen for CHIP tracks (DECISIONS D-61): the wave as it is
// now, drawn from the table; the settings that make it; then the filter and
// the amp envelope (drawn and placed as on the other instruments), and playing.

#include <cmath>
#include <cstdio>

#include "engine/chip/chip_engine.h"
#include "engine/envelope_params.h"
#include "engine/module.h"
#include "project/session.h"
#include "ui/envelope_view.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;
using C = ChipEngine;

// ↑↓ order: the wave, the amp envelope (straight after, as on FM), the
// filter and rate, then playing.
const int kRows[] = {C::kShape,     C::kSize,    C::kMult,     C::kWarp,       C::kMirror,     C::kAmp,    C::kLimit,  C::kBits,
                     C::kAmpAttack, C::kAmpHold, C::kAmpDecay, C::kAmpSustain, C::kAmpRelease, C::kVelAmp,
                     C::kFilter,    C::kCutoff,  C::kResonance, C::kRate,
                     C::kTune,      C::kFine,    C::kGlide,    C::kPoly,       C::kBendRange,  C::kLevel};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);
constexpr int kWaveRows = 8, kAmpRows = 6, kToneRows = 4;

class ChipScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const Session& s = ctx.session;
    const int t = ctx.track;
    const int inner = std::max(24, ctx.width - 4);
    const bool twoColumns = inner >= 56;
    const int colW = twoColumns ? (inner - 2) / 2 : inner;
    const auto table = C::paramTable();
    auto v = [&](int p) { return s.param(engineParamRef(t, p)); };

    auto cell = [&](int r, int w) {
      const int param = kRows[r];
      const ParamInfo& pi = table[static_cast<std::size_t>(param)];
      const bool on = r == row_;
      const float val = v(param);
      Elements cells = {text(on ? "▸ " : "  ") | color(palette::accent),
                        text(fit(pi.label, 11)) | size(WIDTH, EQUAL, 11) | color(on ? palette::accent : palette::text)};
      // Bars on the wave and tone settings; the envelope and playing rows are
      // plain values, as on the other instruments.
      const bool continuous = pi.curve != Curve::Choice && pi.curve != Curve::Toggle;
      const bool barred = r < kWaveRows || (r >= kWaveRows + kAmpRows && r < kWaveRows + kAmpRows + kToneRows);
      if (continuous && barred && w >= 34) {
        cells.push_back(valueBar(toNormalized(pi, val), std::clamp(w - 26, 6, 12), on ? palette::accent : palette::chip, false,
                                 modulatedAt(ctx, engineParamRef(t, param))));
        cells.push_back(text(" "));
      }
      cells.push_back(text(formatValue(pi, val)) | color(on ? palette::text : palette::dim));
      cells.push_back(filler());
      Element e = hbox(cells);
      return ctx.value(on ? e | bgcolor(palette::raised) : e, [this, r](Context&) { row_ = r; });
    };

    // The wave, from a drawing copy of the engine's table (the audio thread owns the real one).
    ChipEngine copy;
    for (int p = 0; p <= C::kBits; ++p) copy.setParam(p, v(p));
    copy.rebuildIfNeeded();
    const auto wave = copy.table();
    const int shape = static_cast<int>(std::lround(v(C::kShape)));
    const bool noise = shape == C::kNoiseTone || shape == C::kNoise;
    const int waveW = std::min(inner, 64), waveH = 4;
    Elements waveRows;
    static const char* blocks[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    for (int row = waveH - 1; row >= 0; --row) {
      std::string line;
      for (int x = 0; x < waveW; ++x) {
        float y = noise ? 0.0f : wave[static_cast<std::size_t>(x * static_cast<int>(wave.size()) / waveW)];
        if (noise) {
          uint32_t h = static_cast<uint32_t>(x) * 2654435761u;
          y = static_cast<float>((h >> 16) & 255) / 127.5f - 1.0f;
        }
        const float level = (y + 1.0f) * 0.5f * waveH;  // 0 … waveH
        const float in = std::clamp(level - static_cast<float>(row), 0.0f, 1.0f);
        line += blocks[static_cast<int>(std::lround(in * 8))];
      }
      waveRows.push_back(text(line) | color(palette::chip));
    }
    char about[96];
    std::snprintf(about, sizeof about, "%s · %d steps · %d bit%s", noise ? "noise" : "wave", noise ? 0 : static_cast<int>(wave.size()),
                  static_cast<int>(std::lround(v(C::kBits))), std::lround(v(C::kBits)) == 1 ? "" : "s");
    Elements lines = {vbox(waveRows) | bgcolor(palette::surface), text(noise ? "a shift register, pitched by the note" : about) | color(palette::faint), text("")};

    // Wave settings in two columns, then amp beside tone, then playing.
    auto columns = [&](const Elements& a, const Elements& b) {
      for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        lines.push_back(hbox({(i < a.size() ? a[i] : text("")) | size(WIDTH, EQUAL, colW), text("  "),
                              (i < b.size() ? b[i] : text("")) | size(WIDTH, EQUAL, colW)}));
    };
    Elements w1, w2;
    for (int r = 0; r < kWaveRows; ++r) (twoColumns && r >= kWaveRows / 2 ? w2 : w1).push_back(cell(r, colW));
    std::array<float, kEnvParamCount> env{};
    for (int k = 0; k < kEnvParamCount; ++k) env[static_cast<std::size_t>(k)] = v(C::kAmpAttack + k);
    Elements amp = {hbox({text("amp ") | color(palette::dim), text(envelopeShape(env.data(), std::max(6, colW - 6))) | color(palette::value)})};
    for (int r = kWaveRows; r < kWaveRows + kAmpRows; ++r) amp.push_back(cell(r, colW));
    Elements tone = {text("filter · rate") | color(palette::dim)};
    for (int r = kWaveRows + kAmpRows; r < kWaveRows + kAmpRows + kToneRows; ++r) tone.push_back(cell(r, colW));
    Elements play = {text("play") | color(palette::dim)};
    for (int r = kWaveRows + kAmpRows + kToneRows; r < kRowCount; ++r) play.push_back(cell(r, colW));
    if (twoColumns) {
      columns(w1, w2);
      lines.push_back(text(""));
      columns(amp, tone);
      lines.push_back(text(""));
      columns(play, {});
    } else {
      for (auto* group : {&w1, &amp, &tone, &play}) {
        for (Element& e : *group) lines.push_back(e);
        lines.push_back(text(""));
      }
    }
    // Short screens: keep the focused row's part in view (the wave goes first).
    const int room = std::max(4, ctx.height - 3 - 2 - 2);
    if (static_cast<int>(lines.size()) > room) {
      const int at = row_ < kWaveRows ? 4 : row_ < kWaveRows + kAmpRows + kToneRows ? 10 : static_cast<int>(lines.size()) - 1;
      const int first = std::clamp(at - room / 2, 0, static_cast<int>(lines.size()) - room);
      lines = Elements(lines.begin() + first, lines.begin() + first + room);
    }
    lines.push_back(filler());
    lines.push_back(separatorLight() | color(palette::borderSubtle));
    lines.push_back(text(fit("l learns a CC · v modulates a value (an envelope on tune makes a pitch sweep)", inner)) | color(palette::faint));
    return pane("Track " + std::to_string(t + 1) + " · CHIP · lo-fi wave synth", vbox(lines), ctx.piano ? palette::value : palette::accent) | flex;
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
    return {{"↑↓", "Setting"}, {"←→", "Change"}, {"l", "Learn"}, {"v", "Modulate"}, {"⏎", "Play"}, {"Esc", "Back"}, {"?", "Help"}};
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override { return engineParamRef(ctx.track, kRows[row_]); }

 private:
  int row_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeChipScreen() { return std::make_unique<ChipScreen>(); }

}  // namespace ion::ui
