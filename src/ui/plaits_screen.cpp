// The Instrument screen for PLAITS tracks (mockups/Plaits, HANDOFF §6.4,
// DECISIONS D-56, D-57). Left: the 24 models in a 3 × 8 grid by bank, or the
// presets. Right: the three macros, described for the model, then the voice,
// the LPG and the envelope.

#include <cmath>
#include <cstdio>

#include "engine/module.h"
#include "engine/plaits/plaits_engine.h"
#include "library/fm_library.h"
#include "project/session.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;
using P = PlaitsEngine;

struct Bank {
  const char* name;
  const char* about;
  Color colour;
};
const Bank kBanks[3] = {{"green", "the originals", palette::green},
                        {"red", "noise and physical models", palette::record},
                        {"orange", "firmware 1.2", palette::accent}};

// The right pane's rows, in order. Groups start with a heading.
struct Row {
  int param;
  const char* group;  // a heading drawn above it, or null
};
const Row kRows[] = {
    {P::kHarmonics, nullptr}, {P::kTimbre, nullptr},      {P::kMorph, nullptr},
    {P::kLpgMode, "voice"},   {P::kLpgDecay, nullptr},    {P::kLpgColour, nullptr}, {P::kAux, nullptr},
    {P::kVelLpg, nullptr},    {P::kPoly, nullptr},        {P::kGlide, nullptr},     {P::kBendRange, nullptr},
    {P::kEnvPitch, "decay envelope →"}, {P::kEnvTimbre, nullptr}, {P::kEnvMorph, nullptr},
    {P::kAmpAttack, "amp envelope (after the LPG)"}, {P::kAmpHold, nullptr}, {P::kAmpDecay, nullptr},
    {P::kAmpSustain, nullptr}, {P::kAmpRelease, nullptr}, {P::kVelAmp, nullptr},
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);

Element signedBar(float v, int width, bool focused) {
  const int half = width / 2;
  const int n = static_cast<int>(std::lround(std::min(1.0f, std::fabs(v)) * static_cast<float>(half)));
  std::string left, right;
  for (int i = 0; i < half; ++i) left += (v < 0 && i >= half - n) ? "█" : "░";
  for (int i = 0; i < width - half; ++i) right += (v > 0 && i < n) ? "█" : "░";
  const Color on = focused ? palette::accent : palette::green;
  return hbox({text(left) | color(v < 0 ? on : palette::border), text(right) | color(v > 0 ? on : palette::border)});
}

class PlaitsScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    // Below 110 columns one pane at a time (Tab swaps): the grid needs the width.
    if (!ctx.wide()) return (pane_ == 0 ? left(ctx, ctx.width) : params(ctx, ctx.width)) | flex;
    const int leftW = std::clamp(ctx.width * 46 / 100, 50, 64);
    return hbox({left(ctx, leftW) | size(WIDTH, EQUAL, leftW), params(ctx, ctx.width - leftW) | flex});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    const int t = ctx.track;
    if (presets_) return presetEvent(ctx, e);
    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = 1 - pane_;
      return true;
    }
    if (e == Event::Character('o')) {
      presetList_ = s.listPlaitsPresets();
      presets_ = true;
      preset_ = 0;
      pane_ = 0;
      if (presetList_.empty()) ctx.flash = "no Plaits presets yet: a saves one";
      return true;
    }
    if (e == Event::Character('a')) {
      startSave(ctx);
      return true;
    }
    if (e == Event::Character('f')) {
      fillFromFavourites(ctx);
      return true;
    }
    if (e == Event::Character('x') && !s.track(t).plaitsPatches.empty()) {
      s.setPlaitsPatches(t, {});
      ctx.flash = "6-op FM a plays Plaits' own bank again";
      return true;
    }
    if (pane_ == 0) {
      int m = model(ctx);
      int bank = m / 8, i = m % 8;
      if (e == Event::ArrowUp) i = (i + 7) % 8;
      else if (e == Event::ArrowDown) i = (i + 1) % 8;
      else if (e == Event::ArrowLeft) bank = (bank + 2) % 3;
      else if (e == Event::ArrowRight) bank = (bank + 1) % 3;
      else if (e == Event::Return) {
        ctx.request = Context::Request::Audition;
        return true;
      } else {
        return false;
      }
      setModel(ctx, bank * 8 + i);
      return true;
    }
    if (e == Event::ArrowUp) { row_ = (row_ + kRowCount - 1) % kRowCount; return true; }
    if (e == Event::ArrowDown) { row_ = (row_ + 1) % kRowCount; return true; }
    if (e == Event::Return) { ctx.request = Context::Request::Audition; return true; }
    if (const Nudge n = nudgeOf(e); n.direction) {
      s.nudgeParam(engineParamRef(t, kRows[row_].param), n.direction, n.coarse);
      return true;
    }
    return false;
  }

  Hints hints(Context& ctx) override {
    if (presets_) return {{"↑↓", "Preset"}, {"⏎", "Load"}, {"Esc", "Models"}};
    Hints h;
    if (pane_ == 0) h = {{"↑↓←→", "Model"}, {"⏎", "Play"}, {"Tab", "Params"}};
    else h = {{"↑↓", "Param"}, {"←→", "Change"}, {"l", "Learn"}, {"Tab", "Models"}};
    h.push_back({"o", "Presets"});
    h.push_back({"a", "Save preset"});
    if (P::isSixOp(model(ctx)) || !ctx.narrow()) h.push_back({"f", "6-op list"});
    h.push_back({"Spc", "Audition"});
    h.push_back({"Esc", "Back"});
    return h;
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    if (pane_ == 0) return engineParamRef(ctx.track, P::kModel);
    return engineParamRef(ctx.track, kRows[row_].param);
  }

 private:
  int model(const Context& ctx) const {
    return std::clamp(static_cast<int>(std::lround(ctx.session.param(engineParamRef(ctx.track, P::kModel)))), 0, P::kModels - 1);
  }
  void setModel(Context& ctx, int m) { ctx.session.setParam(engineParamRef(ctx.track, P::kModel), static_cast<float>(m)); }

  // What Harmonics is pointing at, on the models where it picks something.
  std::string harmonicsPick(const Context& ctx, int m) const {
    const Session& s = ctx.session;
    const float h = s.param(engineParamRef(ctx.track, P::kHarmonics));
    if (P::isChord(m)) return P::chordName(h);
    if (!P::isSixOp(m)) return {};
    const int slot = P::sixOpSlot(h);
    const auto& list = s.track(ctx.track).plaitsPatches;
    if (P::isUserBank(m) && !list.empty()) return syx::name(list[static_cast<std::size_t>(slot) * list.size() / 32]);
    return P::builtInPatchName(m - 18, slot);
  }

  // ---- left: the models (or the presets) ----

  Element left(Context& ctx, int width) {
    if (presets_) return presetPane(ctx);
    const int m = model(ctx);
    const bool focused = pane_ == 0;
    const int colW = std::max(12, (width - 4) / 3);
    Elements cols;
    for (int b = 0; b < 3; ++b) {
      Elements rows = {hbox({text("● ") | color(kBanks[b].colour), text(kBanks[b].name) | color(palette::dim)})};
      for (int i = 0; i < 8; ++i) {
        const int index = b * 8 + i;
        const bool on = index == m;
        Element r = hbox({text(std::to_string(i + 1) + " ") | color(on ? palette::bg : palette::faint),
                          text(fit(P::modelName(index), colW - 3)) | color(on ? palette::bg : palette::text), filler()});
        if (on) r = r | bgcolor(focused ? palette::accent : palette::raised);
        rows.push_back(ctx.item(r | size(WIDTH, EQUAL, colW), [this, index](Context& c) {
          pane_ = 0;
          setModel(c, index);
        }, [this](Context&) { pane_ = 0; }));
      }
      cols.push_back(vbox(rows));
      if (b < 2) cols.push_back(text(" "));
    }
    Elements out = {hbox(cols), text("")};
    out.push_back(text(fit("24 models · 6-op FM models draw on your FM library", width - 4)) | color(palette::faint));
    out.push_back(text(""));
    out.push_back(hbox({text(P::modelName(m)) | bold | color(palette::green), text("  "),
                        text("● " + std::string(kBanks[m / 8].name)) | color(kBanks[m / 8].colour)}));
    out.push_back(paragraph(P::modelAbout(m)) | color(palette::dim));
    out.push_back(text(""));
    static const char* macros[] = {"harmonics", "timbre", "morph"};
    for (int k = 0; k < 3; ++k)
      out.push_back(hbox({text(std::string(macros[k]) + "  ") | size(WIDTH, EQUAL, 11) | color(palette::faint),
                          text(fit(P::macroRole(m, k), width - 15)) | color(palette::text)}));
    if (P::isSixOp(m)) {
      out.push_back(text(""));
      const auto& list = ctx.session.track(ctx.track).plaitsPatches;
      if (P::isUserBank(m))
        out.push_back(paragraph(list.empty() ? "plays Plaits' own bank: f fills it from your FM favourites (up to 32)"
                                             : std::to_string(list.size()) + " patches from your list · f refills · x clears") |
                      color(palette::dim));
      else
        out.push_back(paragraph("Plaits' own bank · your list plays on 6-op FM a") | color(palette::faint));
      out.push_back(text("harmonics → " + harmonicsPick(ctx, m)) | color(palette::value));
    }
    out.push_back(filler());
    return pane("Models", vbox(out), ctx.piano ? palette::value : focused ? palette::accent : palette::border);
  }

  Element presetPane(Context& ctx) {
    Elements rows;
    for (int i = 0; i < static_cast<int>(presetList_.size()); ++i) {
      const bool on = i == preset_;
      Element r = text(" " + presetList_[static_cast<std::size_t>(i)] + " ");
      if (on) r = r | color(palette::bg) | bgcolor(palette::accent);
      rows.push_back(ctx.item(r, [this, i](Context&) { preset_ = i; }));
    }
    if (presetList_.empty()) rows.push_back(text(" none yet: a saves the track's settings as one") | color(palette::faint));
    rows.push_back(filler());
    rows.push_back(text(" " + abbreviateHome((ctx.session.paths().presetsDir / "plaits").string())) | color(palette::faint));
    return pane("Plaits presets", vbox(rows), palette::accent);
  }

  bool presetEvent(Context& ctx, const Event& e) {
    const int n = static_cast<int>(presetList_.size());
    if (e == Event::Escape || e == Event::Character('o')) {
      presets_ = false;
      return true;
    }
    if (n == 0) return e != Event::Tab;
    if (e == Event::ArrowUp) { preset_ = (preset_ + n - 1) % n; return true; }
    if (e == Event::ArrowDown) { preset_ = (preset_ + 1) % n; return true; }
    if (e == Event::Return) {
      std::string error;
      const std::string& name = presetList_[static_cast<std::size_t>(std::clamp(preset_, 0, n - 1))];
      if (ctx.session.loadPlaitsPreset(ctx.track, name, error)) {
        ctx.flash = "preset " + name + " → track " + std::to_string(ctx.track + 1);
        presets_ = false;
      } else {
        ctx.flash = "not loaded: " + error;
      }
      return true;
    }
    return true;
  }

  void startSave(Context& ctx) {
    const int t = ctx.track;
    ctx.promptTitle = "Save Plaits preset as (the same name replaces it)";
    ctx.promptInitial = P::modelName(model(ctx));
    ctx.promptDone = [t](Context& cx, const std::string& name) {
      std::string error;
      if (cx.session.savePlaitsPreset(t, name, error)) cx.flash = "saved preset " + name;
      else cx.flash = "not saved: " + error;
    };
    ctx.request = Context::Request::Prompt;
  }

  void fillFromFavourites(Context& ctx) {
    Session& s = ctx.session;
    const FmLibrary* lib = s.library();
    if (!lib) {
      ctx.flash = "the FM library isn't open";
      return;
    }
    std::vector<syx::Voice> list;
    for (int i : lib->filter({}, {}, true)) {
      if (static_cast<int>(list.size()) >= Session::kMaxPlaitsPatches) break;
      list.push_back(lib->patches()[static_cast<std::size_t>(i)].voice);
    }
    if (list.empty()) {
      ctx.flash = "no FM favourites yet: * on a patch in an FM track's library marks one";
      return;
    }
    s.setPlaitsPatches(ctx.track, std::move(list));
    if (model(ctx) != 18) setModel(ctx, 18);
    ctx.flash = std::to_string(s.track(ctx.track).plaitsPatches.size()) + " favourites → 6-op FM a · harmonics picks one";
  }

  // ---- right: the macros and the rest ----

  Element params(Context& ctx, int width) {
    const Session& s = ctx.session;
    const int t = ctx.track, m = model(ctx);
    const bool focused = pane_ == 1;
    const int inner = std::max(20, width - 4);
    const int labelW = 11, barW = std::clamp(inner - labelW - 30, 8, 16);
    const auto table = P::paramTable();
    Elements lines;
    std::vector<int> rowLine(kRowCount, 0);  // which line each row is on, for scrolling
    for (int r = 0; r < kRowCount; ++r) {
      const Row& row = kRows[r];
      if (row.group) {
        lines.push_back(text(""));
        lines.push_back(text(row.group) | color(palette::dim));
      }
      const ParamInfo& pi = table[static_cast<std::size_t>(row.param)];
      const float v = s.param(engineParamRef(t, row.param));
      const bool on = focused && r == row_;
      Elements cells = {text(on ? "▸ " : "  ") | color(palette::accent),
                        text(fit(pi.label, labelW)) | size(WIDTH, EQUAL, labelW) | color(on ? palette::accent : palette::text)};
      const bool macro = r < 3;
      if (macro) {
        cells.push_back(valueBar(v, barW, on ? palette::accent : palette::green));
        std::string value = formatValue(pi, v);
        if (r == 0) {
          const std::string pick = harmonicsPick(ctx, m);
          if (!pick.empty()) value = pick;
        }
        cells.push_back(text(" " + fit(value, 10)) | size(WIDTH, EQUAL, 11) | color(on ? palette::text : palette::value));
        cells.push_back(text(fit(P::macroRole(m, r), inner - labelW - barW - 14)) | color(palette::dim));
      } else if (row.param == P::kEnvPitch || row.param == P::kEnvTimbre || row.param == P::kEnvMorph) {
        cells.push_back(signedBar(v, barW, on));
        cells.push_back(text(" " + formatValue(pi, v)) | color(on ? palette::text : palette::dim));
      } else {
        cells.push_back(text(formatValue(pi, v)) | color(on ? palette::text : palette::dim));
      }
      cells.push_back(filler());
      Element e = hbox(cells);
      rowLine[static_cast<std::size_t>(r)] = static_cast<int>(lines.size());
      lines.push_back(ctx.value(on ? e | bgcolor(palette::raised) : e, [this, r](Context&) {
        pane_ = 1;
        row_ = r;
      }));
      if (r == 2) {
        // What the LPG is doing, under the macros.
        const int mode = static_cast<int>(std::lround(s.param(engineParamRef(t, P::kLpgMode))));
        static const char* lpg[] = {"LPG gate: opens while the key is held, closes after it",
                                    "LPG ping: every note strikes it, and it dies away by itself",
                                    "LPG off: the model straight out; the amp envelope shapes it"};
        lines.push_back(text(fit(lpg[std::clamp(mode, 0, 2)], inner)) | color(palette::faint));
      }
    }
    // CCs mapped onto this track's Plaits.
    std::string ccs;
    for (const Session::CcEntry& c : s.ccMappings()) {
      if (c.mapping.target.scope != t || !c.mapping.target.isEngine()) continue;
      const int p = c.mapping.target.index - kEngineParamBase;
      if (p < 0 || p >= P::kParamCount) continue;
      std::string label = table[static_cast<std::size_t>(p)].label;
      for (char& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      ccs += (ccs.empty() ? "" : " · ") + ("cc " + std::to_string(c.mapping.cc) + " → " + label);
    }
    Elements footer = {separatorLight() | color(palette::borderSubtle),
                       text(fit(ccs.empty() ? "from the OXI: l on a value learns a CC" : "from the OXI: " + ccs, inner)) |
                           color(ccs.empty() ? palette::faint : palette::value),
                       text(fit("each voice is one Plaits engine · " + std::to_string(ctx.module.status().trackVoices[static_cast<std::size_t>(t)].load()) +
                                    " sounding",
                                inner)) |
                           color(palette::faint)};

    // Scroll so the focused row is in view when it doesn't all fit.
    const int room = std::max(4, ctx.height - 3 - 2 - static_cast<int>(footer.size()));  // header, status, hints; the borders
    int first = 0;
    if (static_cast<int>(lines.size()) > room) {
      const int at = rowLine[static_cast<std::size_t>(row_)];
      first = std::clamp(at - room / 2, 0, static_cast<int>(lines.size()) - room);
    }
    Elements shown(lines.begin() + first, lines.begin() + std::min(static_cast<int>(lines.size()), first + room));
    std::string title = "Track " + std::to_string(t + 1) + " · PLTS · " + P::modelName(m);
    shown.push_back(filler());
    for (Element& f : footer) shown.push_back(f);
    return pane(fit(title, width - 6), vbox(shown), ctx.piano ? palette::value : focused ? palette::accent : palette::border);
  }

  int pane_ = 0;
  int row_ = 0;
  bool presets_ = false;
  int preset_ = 0;
  std::vector<std::string> presetList_;
};

}  // namespace

std::unique_ptr<Screen> makePlaitsScreen() { return std::make_unique<PlaitsScreen>(); }

}  // namespace ion::ui
