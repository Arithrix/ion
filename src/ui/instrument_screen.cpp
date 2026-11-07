// Instrument (I): the selected track's engine. Phase 1 has FM only: browse the
// bank loaded with --syx (Phase 2 replaces it with the full library) and set
// the engine parameters.

#include <cstdio>

#include "engine/module.h"
#include "project/session.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;

class InstrumentScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const Session& s = ctx.session;
    const TrackState& tr = s.track(ctx.track);
    const std::string title = "Track " + std::to_string(ctx.track + 1) + " · " + engineLabel(tr.engine);
    if (tr.engine == EngineType::None)
      return pane(title, vbox({text("This track is empty.") | color(palette::dim),
                               text("⏎ chooses an engine. FM is available now; Drum, Sampler and Plaits arrive in Phases 3, 5 and 7.") |
                                   color(palette::faint)}),
                  true) |
             flex;
    if (tr.engine != EngineType::Fm) return pane(title, text("not built yet") | color(palette::faint), true) | flex;

    Element list = bankList(ctx);
    Element detail = patchDetail(ctx);
    if (ctx.narrow()) return (pane_ == 0 ? list : detail) | flex;
    return hbox({list | flex, detail | size(WIDTH, EQUAL, std::max(36, ctx.width * 42 / 100))});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    const TrackState& tr = s.track(ctx.track);
    if (tr.engine == EngineType::None) {
      if (e == Event::Return) {
        ctx.request = Context::Request::ChooseEngine;
        return true;
      }
      return false;
    }
    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = 1 - pane_;
      return true;
    }
    if (pane_ == 0) {
      // Moving the cursor loads the patch at once (HANDOFF §6.3), so whatever
      // the OXI is playing changes as you scroll.
      if (e == Event::ArrowUp) { s.stepBank(ctx.track, -1); return true; }
      if (e == Event::ArrowDown) { s.stepBank(ctx.track, 1); return true; }
      if (e == Event::PageUp) { s.stepBank(ctx.track, -8); return true; }
      if (e == Event::PageDown) { s.stepBank(ctx.track, 8); return true; }
      return false;
    }
    const auto table = engineParams(tr.engine);
    if (table.empty()) return false;
    if (e == Event::ArrowUp) { row_ = (row_ + static_cast<int>(table.size()) - 1) % static_cast<int>(table.size()); return true; }
    if (e == Event::ArrowDown) { row_ = (row_ + 1) % static_cast<int>(table.size()); return true; }
    if (const Nudge n = nudgeOf(e); n.direction) {
      s.nudgeParam(param(ctx), n.direction, n.coarse);
      return true;
    }
    return false;
  }

  Hints hints(Context& ctx) override {
    if (ctx.session.track(ctx.track).engine == EngineType::None) return {{"⏎", "Engine"}, {"Esc", "Back"}};
    if (pane_ == 1) return {{"↑↓", "Param"}, {"←→", "Change"}, {"Tab", "Bank"}, {"l", "Learn"}, {"Spc", "Audition"}, {"Esc", "Back"}};
    return {{"↑↓", "Patch"}, {"PgUp/Dn", "Jump 8"}, {"Tab", "Params"}, {"Spc", "Audition"}, {"Esc", "Back"}};
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    if (pane_ != 1 || engineParams(ctx.session.track(ctx.track).engine).empty()) return std::nullopt;
    return param(ctx);
  }

 private:
  ParamRef param(Context& ctx) const {
    return {static_cast<uint8_t>(ctx.track), static_cast<uint8_t>(kEngineParamBase + row_)};
  }

  Element bankList(Context& ctx) {
    const Session& s = ctx.session;
    const auto& bank = s.bank();
    const std::string title = bank.empty() ? "Bank" : "Bank · " + s.bankName();
    if (bank.empty())
      return pane(title,
                  vbox({text("No bank loaded.") | color(palette::dim),
                        text("Start ion with --syx FILE to browse a DX7 bank here.") | color(palette::faint),
                        text("The full FM library (import, search, favourites) is Phase 2.") | color(palette::faint)}),
                  pane_ == 0);
    const int sel = std::max(0, s.track(ctx.track).bankIndex);
    const int visible = std::max(4, ctx.height - 9);
    const int first = std::clamp(sel - visible / 2, 0, std::max(0, static_cast<int>(bank.size()) - visible));
    Elements rows;
    for (int i = first; i < std::min(static_cast<int>(bank.size()), first + visible); ++i) {
      const syx::Voice& v = bank[static_cast<std::size_t>(i)];
      char num[16];
      std::snprintf(num, sizeof num, "%3d  ", i + 1);
      char alg[16];
      std::snprintf(alg, sizeof alg, "alg %2d", v[134] + 1);
      const bool on = i == s.track(ctx.track).bankIndex;
      Element row = hbox({text(num) | color(on ? palette::bg : palette::dim), text(syx::name(v)) | flex,
                          text(alg) | color(on ? palette::bg : palette::dim), text(" ")});
      rows.push_back(on ? row | color(palette::bg) | bgcolor(pane_ == 0 ? palette::accent : palette::raised) : row);
    }
    rows.push_back(filler());
    rows.push_back(paragraph("moving the cursor loads the patch · held notes stop (Phase 2 fixes that)") | color(palette::faint));
    return pane(title, vbox(rows), ctx.piano ? palette::value : (pane_ == 0 ? palette::accent : palette::border));
  }

  Element patchDetail(Context& ctx) {
    const Session& s = ctx.session;
    const TrackState& tr = s.track(ctx.track);
    Elements out;
    if (tr.hasVoice) {
      const syx::Voice& v = tr.voice;
      out.push_back(text(syx::name(v)) | bold | color(palette::value));
      out.push_back(text("from " + (tr.voiceSource.empty() ? std::string("project") : tr.voiceSource)) | color(palette::dim));
      out.push_back(text(""));
      out.push_back(paramRow("algorithm", std::to_string(v[134] + 1), false, 12));
      out.push_back(paramRow("feedback", std::to_string(v[135]), false, 12));
      const int transpose = static_cast<int>(v[144]) - 24;
      out.push_back(paramRow("transpose", (transpose > 0 ? "+" : "") + std::to_string(transpose) + " st", false, 12));
    } else {
      out.push_back(text("INIT VOICE") | bold | color(palette::value));
      out.push_back(text("Dexed's initial voice") | color(palette::dim));
    }
    out.push_back(text(""));
    out.push_back(text("engine") | color(palette::dim));
    const auto table = engineParams(tr.engine);
    for (std::size_t i = 0; i < table.size(); ++i) {
      const ParamRef ref{static_cast<uint8_t>(ctx.track), static_cast<uint8_t>(kEngineParamBase + i)};
      out.push_back(paramRow(table[i].label, formatValue(table[i], s.param(ref)), pane_ == 1 && static_cast<int>(i) == row_, 12));
    }
    out.push_back(text(""));
    out.push_back(paragraph("macros (bright, attack, release, feedback, detune, velocity) arrive in Phase 2") | color(palette::faint));
    out.push_back(filler());
    return pane("Patch", vbox(out), pane_ == 1 ? palette::accent : palette::border);
  }

  int pane_ = 0;
  int row_ = 0;
};

class LibraryScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    Session& s = ctx.session;
    projects_ = s.listProjects();
    sel_ = std::clamp(sel_, 0, std::max(0, static_cast<int>(projects_.size()) - 1));
    Elements rows;
    rows.push_back(hbox({text("   name") | flex, text("saved  ")}) | color(palette::dim));
    for (int i = 0; i < static_cast<int>(projects_.size()); ++i) {
      const std::string& name = projects_[static_cast<std::size_t>(i)];
      const bool current = name == s.name();
      const bool on = i == sel_;
      Element row = hbox({text(current ? " ● " : "   ") | color(on ? palette::bg : palette::accent), text(name) | flex,
                          text(s.hasRecovery(name) ? "autosave " : "") | color(on ? palette::bg : palette::record)});
      rows.push_back(on ? row | color(palette::bg) | bgcolor(palette::accent) : row);
    }
    if (projects_.empty()) rows.push_back(text("   no saved projects yet: w saves this one") | color(palette::faint));
    rows.push_back(filler());
    rows.push_back(text(" " + s.paths().projectsDir.string()) | color(palette::faint));
    Element list = pane("Projects", vbox(rows), true);

    Elements info = {
        text(s.name()) | bold | color(palette::value),
        text(s.dirty() ? "● unsaved changes" : "saved") | color(s.dirty() ? palette::accent : palette::dim),
        text(""),
        paragraph("⏎ loads the selected project") | color(palette::dim),
        paragraph("n starts a new one · w saves · W saves under a new name") | color(palette::dim),
        text(""),
        paragraph("FM patches, samples, kits and recordings join this screen in Phase 2.") | color(palette::faint),
    };
    if (pending_ != Action::None)
      info.push_back(text(std::string("unsaved changes: ") + (pending_ == Action::New ? "n" : "⏎") +
                          " again to discard them, Esc to cancel") |
                     color(palette::record));
    Element detail = pane("Project", vbox(info), false);
    if (ctx.narrow()) return list | flex;
    return hbox({list | flex, detail | size(WIDTH, EQUAL, std::max(36, ctx.width * 42 / 100))});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    if (e == Event::ArrowUp) { sel_ = std::max(0, sel_ - 1); pending_ = Action::None; return true; }
    if (e == Event::ArrowDown) { sel_ = std::min(static_cast<int>(projects_.size()) - 1, sel_ + 1); pending_ = Action::None; return true; }
    if (e == Event::Escape && pending_ != Action::None) {
      pending_ = Action::None;
      return true;
    }
    Action act = Action::None;
    if (e == Event::Return && !projects_.empty()) act = Action::Load;
    if (e == Event::Character('n')) act = Action::New;
    if (act == Action::None) return false;
    // With unsaved changes, the same key twice confirms discarding them.
    if (s.dirty() && pending_ != act) {
      pending_ = act;
      return true;
    }
    pending_ = Action::None;
    if (act == Action::New) {
      s.newProject(s.freshName());
      ctx.flash = "new project: " + s.name();
      return true;
    }
    const std::string name = projects_[static_cast<std::size_t>(sel_)];
    std::string error;
    if (s.hasRecovery(name) ? s.recover(name, error) : s.load(name, error))
      ctx.flash = (s.dirty() ? "recovered unsaved changes in " : "loaded ") + name;
    else
      ctx.flash = "can't load: " + error;
    return true;
  }

  Hints hints(Context&) override {
    return {{"↑↓", "Move"}, {"⏎", "Load"}, {"n", "New"}, {"w", "Save"}, {"W", "Save as"}, {"Esc", "Back"}};
  }

 private:
  enum class Action { None, Load, New };
  std::vector<std::string> projects_;
  int sel_ = 0;
  Action pending_ = Action::None;
};

class PlaceholderScreen final : public Screen {
 public:
  PlaceholderScreen(std::string title, std::string message) : title_(std::move(title)), message_(std::move(message)) {}
  Element render(Context&) override {
    return pane(title_, vbox({text(message_) | color(palette::dim), filler()}), true) | flex;
  }
  bool onEvent(Context&, const Event&) override { return false; }
  Hints hints(Context&) override { return {{"T", "Tracks"}, {"?", "Help"}, {"q", "Quit"}}; }

 private:
  std::string title_, message_;
};

}  // namespace

std::unique_ptr<Screen> makeInstrumentScreen() { return std::make_unique<InstrumentScreen>(); }
std::unique_ptr<Screen> makeLibraryScreen() { return std::make_unique<LibraryScreen>(); }
std::unique_ptr<Screen> makePlaceholderScreen(std::string title, std::string message) {
  return std::make_unique<PlaceholderScreen>(std::move(title), std::move(message));
}

}  // namespace ion::ui
