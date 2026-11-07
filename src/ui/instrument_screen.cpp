// Instrument (I): the selected track's engine. For FM, the library browser
// (mockups/FMBrowser, CometFM; HANDOFF §6.3): one flat, filterable list where
// moving the cursor loads the patch, and the patch pane with its algorithm,
// macros and save/export.

#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

#include "engine/fm/dx7_voice.h"
#include "engine/fm/fm_engine.h"
#include "engine/module.h"
#include "library/fm_library.h"
#include "project/paths.h"
#include "project/session.h"
#include "ui/algorithm_diagram.h"
#include "ui/fm_browse.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;

// What each macro does, in a few words (the mockup's note column).
constexpr const char* kMacroNotes[dx7::kMacroCount] = {"modulator levels",  "all envelope attacks", "all envelope releases",
                                                        "the patch feedback", "spread between ops",   "touch on carriers"};
constexpr const char* kMacroShort[dx7::kMacroCount] = {"brt", "atk", "rel", "fb", "det", "vel"};
constexpr int kMacroRows = dx7::kMacroCount + 3;  // + polyphony, bend range, mono

std::string thousands(std::size_t n) {
  std::string s = std::to_string(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
  return s;
}

// A bar centred on zero for a signed value: −1 … +1.
Element signedBar(float v, int width, bool focused) {
  const int half = width / 2;
  const int n = static_cast<int>(std::lround(std::min(1.0f, std::fabs(v)) * static_cast<float>(half)));
  std::string left, right;
  for (int i = 0; i < half; ++i) left += (v < 0 && i >= half - n) ? "█" : "░";
  for (int i = 0; i < width - half; ++i) right += (v > 0 && i < n) ? "█" : "░";
  const Color on = focused ? palette::accent : palette::value;
  return hbox({text(left) | color(v < 0 ? on : palette::borderSubtle), text(right) | color(v > 0 ? on : palette::borderSubtle)});
}

std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> out;
  std::string w;
  for (char c : s + " ") {
    if (c == ' ' || c == ',') {
      if (!w.empty()) out.push_back(w);
      w.clear();
    } else {
      w += c;
    }
  }
  return out;
}

class InstrumentScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const Session& s = ctx.session;
    const TrackState& tr = s.track(ctx.track);
    const std::string title = "Track " + std::to_string(ctx.track + 1) + " · " + engineLabel(tr.engine);
    if (tr.engine == EngineType::None)
      return pane(title, vbox({text("This track is empty.") | color(palette::dim),
                               paragraph("⏎ chooses an engine. FM, Drum and Sampler are available now; Plaits arrives in Phase 7.") |
                                   color(palette::faint)}),
                  true) |
             flex;
    if (tr.engine == EngineType::Drum) return drum_->render(ctx);
    if (tr.engine == EngineType::Sampler) return sampler_->render(ctx);
    if (tr.engine != EngineType::Fm) return pane(title, text("not built yet") | color(palette::faint), true) | flex;

    if (ctx.narrow()) return (pane_ == 0 ? list(ctx) : macroPane(ctx)) | flex;
    const int detailWidth = std::max(44, ctx.width * 45 / 100);
    return hbox({list(ctx) | flex, patchPane(ctx, detailWidth) | size(WIDTH, EQUAL, detailWidth)});
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
    if (tr.engine == EngineType::Drum) return drum_->onEvent(ctx, e);
    if (tr.engine == EngineType::Sampler) return sampler_->onEvent(ctx, e);
    if (tr.engine != EngineType::Fm) return false;
    FmBrowse& fm = *ctx.fm;

    if (fm.typing) {
      if (e == Event::Return) { fm.typing = false; return true; }
      if (e == Event::Escape) { fm.typing = false; fm.setFilter({}); return true; }
      if (e == Event::Backspace) {
        std::string f = fm.filter;
        if (!f.empty()) f.pop_back();
        fm.setFilter(f);
        return true;
      }
      if (e.is_character()) {
        fm.setFilter(fm.filter + e.character());
        return true;
      }
      if (e == Event::ArrowUp || e == Event::ArrowDown) fm.typing = false;  // and fall through to move
    }

    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = 1 - pane_;
      return true;
    }
    // Keys that work in either pane.
    if (e == Event::Return) { ctx.request = Context::Request::Audition; return true; }
    if (e == Event::Character('a')) { startSaveAs(ctx); return true; }
    if (e == Event::Character('u')) {
      if (s.macros(ctx.track).any()) {
        s.resetMacros(ctx.track);
        ctx.flash = "macros back to 0";
      }
      return true;
    }
    if (e == Event::Character('i')) { startImport(ctx); return true; }

    if (pane_ == 1) {
      if (e == Event::ArrowUp) { row_ = (row_ + kMacroRows - 1) % kMacroRows; return true; }
      if (e == Event::ArrowDown) { row_ = (row_ + 1) % kMacroRows; return true; }
      if (const Nudge n = nudgeOf(e); n.direction) {
        s.nudgeParam(param(ctx), n.direction, n.coarse);
        return true;
      }
      return false;
    }

    const auto& items = fm.items(s);
    const int visible = listRows(ctx);
    auto moved = [&](bool ok) {
      if (ok && s.config().previewOnMove) ctx.request = Context::Request::Audition;
      return true;
    };
    if (e == Event::ArrowUp) return moved(fm.move(s, ctx.track, -1));
    if (e == Event::ArrowDown) return moved(fm.move(s, ctx.track, 1));
    if (e == Event::PageUp) return moved(fm.move(s, ctx.track, -visible));
    if (e == Event::PageDown) return moved(fm.move(s, ctx.track, visible));
    if (e == Event::Home) return moved(fm.move(s, ctx.track, -static_cast<int>(items.size())));
    if (e == Event::End) return moved(fm.move(s, ctx.track, static_cast<int>(items.size())));
    if (const Nudge n = nudgeOf(e); n.direction) {
      fm.setView(fm.view + n.direction);
      return true;
    }
    if (e == Event::Character('/')) { fm.typing = true; return true; }
    if (e == Event::Escape && !fm.filter.empty()) { fm.setFilter({}); return true; }

    const int cur = fm.cursor(s, ctx.track);
    const FmBrowse::Item* item = cur >= 0 ? &items[static_cast<std::size_t>(cur)] : nullptr;
    FmLibrary* lib = s.library();
    if (e == Event::Character('*')) {
      if (item && item->patch && lib) lib->setFavourite(item->patch->id, !item->patch->favourite);
      else ctx.flash = "only library patches can be favourites";
      return true;
    }
    if (e == Event::Character('t')) {
      if (!item || !item->patch || !lib) {
        ctx.flash = "only library patches can be tagged";
        return true;
      }
      const int64_t id = item->patch->id;
      std::string current;
      for (const std::string& t : item->patch->tags) current += (current.empty() ? "" : " ") + t;
      ctx.promptTitle = "Tags for " + item->patch->name + " (separate with spaces)";
      ctx.promptInitial = current;
      ctx.promptDone = [id](Context& cx, const std::string& text) {
        if (FmLibrary* l = cx.session.library()) l->setTags(id, splitWords(text));
      };
      ctx.request = Context::Request::Prompt;
      return true;
    }
    if (e == Event::Character('+')) {
      if (!item) return true;
      syx::Voice v;
      std::copy_n(fm.voice(s, *item), v.size(), v.begin());
      const bool had = s.programIndex(v) >= 0;
      const std::string source = item->patch ? item->patch->source : s.programs()[static_cast<std::size_t>(item->program)].source;
      if (!s.toggleProgram(v, source)) ctx.flash = "the program list is full (128)";
      else if (had) ctx.flash = "removed from the program list";
      else ctx.flash = "program " + std::to_string(s.programIndex(v) + 1) + ": " + fm.name(s, *item);
      return true;
    }
    if ((e == kShiftUp || e == kShiftDown) && item && item->program >= 0) {
      s.moveProgram(item->program, e == kShiftUp ? -1 : 1);
      return true;
    }
    if (e == Event::Character('e')) { startExport(ctx); return true; }
    return false;
  }

  Hints hints(Context& ctx) override {
    const TrackState& tr = ctx.session.track(ctx.track);
    if (tr.engine == EngineType::None) return {{"⏎", "Engine"}, {"Esc", "Back"}};
    if (tr.engine == EngineType::Drum) return drum_->hints(ctx);
    if (tr.engine == EngineType::Sampler) return sampler_->hints(ctx);
    if (tr.engine != EngineType::Fm) return {{"Esc", "Back"}};
    if (ctx.fm->typing) return {{"type", "Filter"}, {"⏎", "Done"}, {"Esc", "Clear"}, {"↑↓", "Browse"}};
    if (pane_ == 1)
      return {{"↑↓", "Macro"}, {"←→", "Adjust"}, {"l", "Learn"}, {"u", "Undo edits"}, {"a", "Save as"}, {"⏎", "Audition"},
              {"Tab", "Browse"}, {"Esc", "Back"}};
    Hints h = {{"↑↓", "Browse"}, {"/", "Filter"}, {"←→", "View"}, {"*", "Fav"}, {"t", "Tag"}, {"⏎", "Audition"}, {"Tab", "Macros"}};
    if (!ctx.narrow()) {
      h.push_back({"+", "Program"});
      h.push_back({"a", "Save as"});
      h.push_back({"e", "Export"});
      h.push_back({"i", "Import"});
    }
    h.push_back({"Esc", "Back"});
    return h;
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    if (ctx.session.track(ctx.track).engine == EngineType::Drum) return drum_->focusedParam(ctx);
    if (ctx.session.track(ctx.track).engine == EngineType::Sampler) return sampler_->focusedParam(ctx);
    if (pane_ != 1 || ctx.session.track(ctx.track).engine != EngineType::Fm) return std::nullopt;
    return param(ctx);
  }

  bool capturesText(Context& ctx) override {
    const EngineType type = ctx.session.track(ctx.track).engine;
    if (type == EngineType::Drum) return drum_->capturesText(ctx);
    if (type == EngineType::Sampler) return sampler_->capturesText(ctx);
    return ctx.fm->typing && type == EngineType::Fm;
  }

 private:
  // Macro-pane rows: the six macros first, then the engine settings.
  ParamRef param(Context& ctx) const {
    const int index = row_ < dx7::kMacroCount ? FmEngine::kFirstMacro + row_ : row_ - dx7::kMacroCount;
    return {static_cast<uint8_t>(ctx.track), static_cast<uint16_t>(kEngineParamBase + index)};
  }

  int listRows(const Context& ctx) const { return std::max(4, ctx.height - 12); }

  // ---- the list ----

  Element list(Context& ctx) {
    Session& s = ctx.session;
    FmBrowse& fm = *ctx.fm;
    const FmLibrary* lib = s.library();
    const auto& items = fm.items(s);
    const auto& view = FmBrowse::views()[static_cast<std::size_t>(fm.view)];
    const bool narrow = ctx.narrow();
    const bool wideList = ctx.width >= 110;  // the list pane is ~55% of the width
    const bool focused = pane_ == 0;
    std::string title = "Library · FM";
    if (narrow) title = std::string(pane_ == 0 ? "Browse" : "Macros") + " · Tab ⇄";

    Elements rows;
    // Filter line.
    if (fm.typing)
      rows.push_back(hbox({text("/ ") | color(palette::accent), text(fm.filter) | color(palette::text), text(" ") | bgcolor(palette::accent)}));
    else if (!fm.filter.empty())
      rows.push_back(hbox({text("/ ") | color(palette::accent), text(fm.filter) | color(palette::text),
                           text(narrow ? "" : "   Esc clears") | color(palette::faint)}));
    else
      rows.push_back(text(wideList ? "/ filter by name, tag, category or bank" : "/ filter") | color(palette::faint));

    const std::size_t total = lib ? lib->patches().size() : 0;
    std::string count = thousands(items.size()) + (narrow ? " / " : " of ") + thousands(total);
    if (view.kind == FmBrowse::View::Kind::Programs) count = thousands(items.size()) + " in the program list";
    rows.push_back(hbox({text(count) | color(palette::dim), filler(),
                         text("◀ " + view.label + " ▶") | color(focused ? palette::accent : palette::dim)}));
    if (ctx.importTotal > 0)
      rows.push_back(text("importing… " + std::to_string(ctx.importDone) + " of " + std::to_string(ctx.importTotal) + " files") |
                     color(palette::accent));
    rows.push_back(text(""));

    if (!lib || total == 0) {
      rows.push_back(text(lib ? "The FM library is empty." : "The FM library couldn't be opened.") | color(palette::dim));
      if (lib) {
        rows.push_back(paragraph("i imports DX7 banks: every .syx under a folder, including sub-folders. Duplicates are merged, "
                                 "so import as much as you like.") |
                       color(palette::faint));
      }
      rows.push_back(filler());
      return pane(title, vbox(rows), focused);
    }

    const int nameW = 10, fromW = narrow ? 8 : 12;
    rows.push_back(hbox({text("  name") | size(WIDTH, EQUAL, nameW + 3), text("from") | size(WIDTH, EQUAL, fromW + 1),
                         text(narrow ? "" : "alg") | size(WIDTH, EQUAL, narrow ? 0 : 4), text(narrow ? "" : " pc"), filler()}) |
                   color(palette::faint));
    const int cur = fm.cursor(s, ctx.track);
    const int visible = listRows(ctx);
    const int n = static_cast<int>(items.size());
    const int first = std::clamp(cur - visible / 2, 0, std::max(0, n - visible));
    for (int i = first; i < std::min(n, first + visible); ++i) {
      const FmBrowse::Item& it = items[static_cast<std::size_t>(i)];
      const uint8_t* v = fm.voice(s, it);
      const bool fav = it.patch && it.patch->favourite;
      const bool on = i == cur;
      const int pc = v ? s.programIndex(*reinterpret_cast<const syx::Voice*>(v)) : -1;
      const std::string from = it.patch ? it.patch->source : s.programs()[static_cast<std::size_t>(it.program)].source;
      char alg[8];
      std::snprintf(alg, sizeof alg, "%3d ", v ? v[dx7::kAlgorithmByte] + 1 : 0);
      std::string pcText = pc >= 0 ? (pc < 99 ? " " : "") + std::to_string(pc + 1) : "";
      Element row = hbox({text(fav ? "★ " : "· ") | color(on ? palette::bg : (fav ? palette::accent : palette::faint)),
                          text(fit(fm.name(s, it), nameW)) | size(WIDTH, EQUAL, nameW + 1),
                          text(fit(from, fromW)) | size(WIDTH, EQUAL, fromW + 1) | color(on ? palette::bg : palette::dim),
                          text(narrow ? "" : alg) | color(on ? palette::bg : palette::dim),
                          text(narrow ? "" : pcText) | color(on ? palette::bg : palette::value), filler()});
      if (on) row = row | color(palette::bg) | bgcolor(focused ? palette::accent : palette::raised);
      rows.push_back(row);
    }
    if (items.empty())
      rows.push_back(text(view.kind == FmBrowse::View::Kind::Programs ? "  empty: + adds the highlighted patch"
                                                                      : "  nothing matches")
                     | color(palette::faint));
    rows.push_back(filler());
    if (narrow) {
      // CometFM: the macros as one line under the list.
      const dx7::Macros m = s.macros(ctx.track);
      const int vals[] = {m.bright, m.attack, m.release, m.feedback, m.detune, m.velocity};
      Elements mini;
      for (int i = 0; i < dx7::kMacroCount; ++i) {
        mini.push_back(text(std::string(kMacroShort[i]) + " ") | color(palette::dim));
        const std::string v = vals[i] > 0 ? "+" + std::to_string(vals[i]) : vals[i] < 0 ? "−" + std::to_string(-vals[i]) : "0";
        mini.push_back(text(v + "  ") | color(vals[i] ? palette::accent : palette::text));
      }
      rows.push_back(hbox(mini));
    } else if (wideList) {
      rows.push_back(text(std::string("moving loads the patch · preview on move ") + (s.config().previewOnMove ? "● on" : "○ off") +
                          " (Settings)") |
                     color(palette::faint));
    }
    return pane(title, vbox(rows), ctx.piano ? palette::value : (focused ? palette::accent : palette::border));
  }

  // ---- the patch pane (wide layouts) ----

  Element patchPane(Context& ctx, int width) {
    const Session& s = ctx.session;
    const TrackState& tr = s.track(ctx.track);
    const FmLibrary* lib = s.library();
    const FmPatch* patch = lib && tr.libraryId ? lib->find(tr.libraryId) : nullptr;
    const syx::Voice voice = tr.hasVoice ? tr.voice : syx::initVoice();
    const int alg = voice[dx7::kAlgorithmByte];
    const dx7::Topology& topo = dx7::topology(alg);

    Elements out;
    std::string name = tr.hasVoice ? syx::name(voice) : "INIT VOICE";
    if (name.empty()) name = "(no name)";
    out.push_back(hbox({text(name) | bold | color(palette::value), filler(),
                        text(patch ? patch->category : "") | color(palette::dim)}));
    std::string from = !tr.hasVoice ? "Dexed's initial voice"
                       : patch     ? "from " + patch->source + (patch->sourceCount > 1 ? " · +" + std::to_string(patch->sourceCount - 1) + " more" : "")
                                   : "from " + (tr.voiceSource.empty() ? std::string("this project") : tr.voiceSource) + " · not in the library";
    out.push_back(text(fit(from, width - 2)) | color(palette::dim));
    if (patch) {
      std::string tags;
      for (const std::string& t : patch->tags) tags += (tags.empty() ? "" : " · ") + t;
      out.push_back(text(tags.empty() ? "no tags · t adds some" : "tags " + tags) | color(tags.empty() ? palette::faint : palette::text));
    }

    // Short terminals drop the engine rows and spacing first (they're on
    // Tracks too), then shrink the diagram, then leave it out.
    const int avail = ctx.height - 5;
    const bool roomy = avail >= 22;
    const int room = avail - (roomy ? 22 : 16);
    auto boxed = algorithmDiagram(alg, true);
    auto compact = algorithmDiagram(alg, false);
    const std::vector<std::string>* diagram = static_cast<int>(boxed.size()) <= room   ? &boxed
                                              : static_cast<int>(compact.size()) <= room ? &compact
                                                                                         : nullptr;
    out.push_back(text(""));
    if (diagram)
      for (const std::string& l : *diagram) out.push_back(text("  " + l) | color(palette::text));
    out.push_back(text("algorithm " + std::to_string(alg + 1) + " · " + dx7::describe(alg)) | color(palette::dim));
    const int feedback = voice[dx7::kFeedbackByte];
    std::string fb = "feedback " + std::to_string(feedback) + " on op " + std::to_string(topo.feedbackTo);
    if (topo.feedbackFrom != topo.feedbackTo)
      fb = "feedback " + std::to_string(feedback) + ", loop op " + std::to_string(topo.feedbackFrom) + " → " + std::to_string(topo.feedbackTo);
    out.push_back(text(fb) | color(palette::dim));
    out.push_back(text(""));
    out.push_back(macroRows(ctx, width, roomy));
    out.push_back(filler());
    return pane("Patch", vbox(out), pane_ == 1 ? palette::accent : palette::border);
  }

  // ---- macros (the patch pane's lower half; its own pane when narrow) ----

  Element macroRows(Context& ctx, int width, bool roomy = true) {
    const Session& s = ctx.session;
    const dx7::Macros m = s.macros(ctx.track);
    const bool focused = pane_ == 1;
    const bool notes = width >= 52;
    Elements out;
    out.push_back(text(notes ? "macros · adjust without overwriting the patch" : "macros") | color(palette::dim));
    const auto table = FmEngine::paramTable();
    // Without room, the engine rows go (row_ stays on the macros).
    const int shown = roomy ? kMacroRows : dx7::kMacroCount;
    if (!roomy) row_ = std::min(row_, dx7::kMacroCount - 1);
    for (int i = 0; i < shown; ++i) {
      const int index = i < dx7::kMacroCount ? FmEngine::kFirstMacro + i : i - dx7::kMacroCount;
      const ParamInfo& pi = table[static_cast<std::size_t>(index)];
      const ParamRef ref{static_cast<uint8_t>(ctx.track), static_cast<uint16_t>(kEngineParamBase + index)};
      const float v = s.param(ref);
      const bool on = focused && i == row_;
      if (i == dx7::kMacroCount) out.push_back(text(""));  // only reached when roomy
      if (i < dx7::kMacroCount) {
        const float norm = v / pi.max;
        Element row = hbox({text(on ? "▸ " : "  ") | color(palette::accent), text(pi.label) | size(WIDTH, EQUAL, 10) | color(on ? palette::accent : palette::text),
                            signedBar(norm, 14, on), text(" "), text(formatValue(pi, v)) | size(WIDTH, EQUAL, 4) | color(v != 0 ? palette::accent : palette::text),
                            text(notes ? "  " + std::string(kMacroNotes[i]) : "") | color(palette::faint), filler()});
        out.push_back(on ? row | bgcolor(palette::raised) : row);
      } else {
        Element row = hbox({text(on ? "▸ " : "  ") | color(palette::accent), text(pi.label) | size(WIDTH, EQUAL, 12) | color(on ? palette::accent : palette::dim),
                            text(formatValue(pi, v)) | color(palette::text), filler()});
        out.push_back(on ? row | bgcolor(palette::raised) : row);
      }
    }
    if (roomy) out.push_back(text(""));
    if (m.any()) {
      std::string edits;
      const int vals[] = {m.bright, m.attack, m.release, m.feedback, m.detune, m.velocity};
      for (int i = 0; i < dx7::kMacroCount; ++i)
        if (vals[i]) {
          std::string lower = dx7::kMacroNames[static_cast<std::size_t>(i)];
          for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          edits += (edits.empty() ? "" : ", ") + lower + " " + (vals[i] > 0 ? "+" : "−") + std::to_string(std::abs(vals[i]));
        }
      out.push_back(paragraph("● edited: " + edits) | color(palette::accent));
      out.push_back(text(notes ? "a save as new patch · u undo edits · e export" : "a save as · u undo · e export") | color(palette::dim));
    } else {
      out.push_back(text("○ as stored · a save as · e export") | color(palette::faint));
    }
    // CCs mapped to this track's FM parameters (mockup: "cc 74 → bright").
    std::string ccs;
    for (const Session::CcEntry& c : s.ccMappings())
      if (c.mapping.target.scope == ctx.track && c.mapping.target.isEngine()) {
        const ParamInfo* pi = s.paramInfo(c.mapping.target);
        std::string label = pi ? pi->label : "?";
        for (char& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        ccs += (ccs.empty() ? "" : " · ") + std::string("cc ") + std::to_string(c.mapping.cc) + " → " + label;
      }
    if (!ccs.empty()) out.push_back(text(fit(ccs, width - 2)) | color(palette::value));
    return vbox(out);
  }

  Element macroPane(Context& ctx) {
    const TrackState& tr = ctx.session.track(ctx.track);
    std::string name = tr.hasVoice ? syx::name(tr.voice) : "INIT VOICE";
    return pane("Macros · Tab ⇄", vbox({text(name) | bold | color(palette::value), text(""), macroRows(ctx, ctx.width), filler()}),
                pane_ == 1);
  }

  // ---- actions ----

  void startSaveAs(Context& ctx) {
    const TrackState& tr = ctx.session.track(ctx.track);
    if (!tr.hasVoice) {
      ctx.flash = "load a patch first";
      return;
    }
    if (!ctx.session.library()) {
      ctx.flash = "the FM library isn't open";
      return;
    }
    const int t = ctx.track;
    ctx.promptTitle = "Save as a new patch (10 characters)";
    ctx.promptInitial = syx::name(tr.voice);
    ctx.promptDone = [t](Context& cx, const std::string& name) {
      std::string error;
      if (!cx.session.savePatchAs(t, name, error)) cx.flash = "not saved: " + error;
    };
    ctx.request = Context::Request::Prompt;
  }

  void startExport(Context& ctx) {
    Session& s = ctx.session;
    FmBrowse& fm = *ctx.fm;
    const auto& items = fm.items(s);
    if (items.empty()) {
      ctx.flash = "nothing listed to export";
      return;
    }
    const auto& view = FmBrowse::views()[static_cast<std::size_t>(fm.view)];
    std::string suggested = fm.filter.empty() ? (view.kind == FmBrowse::View::Kind::Category ? view.category
                                                 : view.kind == FmBrowse::View::Kind::All   ? "ion library"
                                                 : view.kind == FmBrowse::View::Kind::Programs ? s.name() + " programs"
                                                 : view.kind == FmBrowse::View::Kind::Favourites ? "favourites"
                                                                                                 : "recent")
                                              : fm.filter;
    const std::size_t banks = (items.size() + 31) / 32;
    ctx.promptTitle = "Export the " + thousands(items.size()) + " listed patches as " + std::to_string(banks) + " .syx bank" +
                      (banks > 1 ? "s" : "") + " — name";
    ctx.promptInitial = suggested;
    std::vector<syx::Voice> voices;
    voices.reserve(items.size());
    for (const auto& it : items) {
      syx::Voice v;
      std::copy_n(fm.voice(s, it), v.size(), v.begin());
      voices.push_back(v);
    }
    ctx.promptDone = [voices = std::move(voices)](Context& cx, const std::string& name) {
      std::vector<std::filesystem::path> written;
      std::string error;
      if (!cx.session.exportVoices(voices, name, written, error)) cx.flash = "not exported: " + error;
      else
        cx.flash = "exported " + std::to_string(voices.size()) + " patches to " + written.front().string() +
                   (written.size() > 1 ? " (+" + std::to_string(written.size() - 1) + " more)" : "");
    };
    ctx.request = Context::Request::Prompt;
  }

  void startImport(Context& ctx) { requestImport(ctx); }

  int pane_ = 0;
  int row_ = 0;
  std::unique_ptr<Screen> drum_ = makeDrumScreen();
  std::unique_ptr<Screen> sampler_ = makeSamplerScreen();
};

}  // namespace

namespace {

std::string trimmed(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  s.erase(0, std::min(s.size(), s.find_first_not_of(" \t")));
  return s;
}

// Asks for the folder; on ⏎ checks it exists before anything runs, and if it
// doesn't, asks again with the path kept so it can be fixed.
void askImportFolder(Context& ctx, const std::string& initial, const std::string& title) {
  ctx.promptTitle = title;
  ctx.promptInitial = initial;
  ctx.promptPath = true;
  ctx.promptDone = [](Context& cx, const std::string& typed) {
    const std::string text = trimmed(typed);
    if (text.empty()) return;
    const std::filesystem::path p = expandHome(text);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
      askImportFolder(cx, text, "Not found: " + abbreviateHome(p.string()) + " · fix the path (Tab completes)");
      return;
    }
    if (std::filesystem::is_regular_file(p, ec)) {
      std::string ext = p.extension().string();
      for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (ext != ".syx" && ext != ".snd") {
        askImportFolder(cx, text, "That's not a .syx file · give a folder or a .syx");
        return;
      }
    }
    cx.importPath = p.string();
    cx.request = Context::Request::Import;
  };
  ctx.request = Context::Request::Prompt;
}

}  // namespace

void requestImport(Context& ctx) {
  if (ctx.importTotal > 0) {
    ctx.flash = "an import is already running";
    return;
  }
  // Start from the last folder if it still exists, else ~/ion/downloads, else ~.
  std::error_code ec;
  std::string initial = ctx.session.config().importFolder;
  if (initial.empty() || !std::filesystem::exists(initial, ec)) {
    const std::filesystem::path downloads = expandHome("~/ion/downloads");
    initial = std::filesystem::is_directory(downloads, ec) ? downloads.string() : expandHome("~/").string();
  }
  initial = abbreviateHome(initial);
  if (!initial.empty() && initial.back() != '/' && std::filesystem::is_directory(expandHome(initial), ec)) initial += '/';
  askImportFolder(ctx, initial, "Import DX7 banks from a folder (sub-folders too) or one .syx · Tab completes");
}

std::unique_ptr<Screen> makeInstrumentScreen() { return std::make_unique<InstrumentScreen>(); }

}  // namespace ion::ui
