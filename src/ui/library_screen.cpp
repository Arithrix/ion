// Library (L): projects, the FM library and drum kits (mockups/Library).
// Recordings join in Phase 6.

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>

#include "engine/drum/drum_engine.h"
#include "library/fm_library.h"
#include "project/paths.h"
#include "project/session.h"
#include "ui/fm_browse.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;

std::string thousands(std::size_t n) {
  std::string s = std::to_string(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
  return s;
}

std::string when(int64_t t) {
  if (!t) return "";
  char buf[32];
  const std::time_t tt = static_cast<std::time_t>(t);
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&tt));
  return buf;
}

class LibraryScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    Session& s = ctx.session;
    projects_ = s.listProjects();
    kits_ = s.listKits();
    sel_ = std::clamp(sel_, 0, std::max(0, rowCount(ctx) - 1));
    if (++frames_ % 30 == 0) {  // pick up saves about once a second
      summaries_.clear();
      kitSummaries_.clear();
    }
    if (ctx.narrow()) return (pane_ == 0 ? sections(ctx) : middle(ctx)) | flex;
    const int detailWidth = std::max(34, ctx.width * 36 / 100);
    return hbox({sections(ctx) | size(WIDTH, EQUAL, 24), middle(ctx) | flex, detail(ctx) | size(WIDTH, EQUAL, detailWidth)});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = 1 - pane_;
      pending_ = Action::None;
      return true;
    }
    if (e == Event::Escape && pending_ != Action::None) {
      pending_ = Action::None;
      return true;
    }
    if (e == Event::Character('i')) {
      requestImport(ctx);
      return true;
    }
    if (pane_ == 0) {
      if (e == Event::ArrowUp) { section_ = (section_ + kSections - 1) % kSections; sel_ = 0; return true; }
      if (e == Event::ArrowDown) { section_ = (section_ + 1) % kSections; sel_ = 0; return true; }
      if (e == Event::Return || e == Event::ArrowRight) { pane_ = 1; return true; }
      return false;
    }
    const int rows = rowCount(ctx);
    if (e == Event::ArrowUp) { sel_ = std::max(0, sel_ - 1); pending_ = Action::None; return true; }
    if (e == Event::ArrowDown) { sel_ = std::min(rows - 1, sel_ + 1); pending_ = Action::None; return true; }
    if (e == Event::ArrowLeft) { pane_ = 0; return true; }

    if (section_ == kFm) {
      if (e == Event::Return) {
        ctx.fm->setView(sel_);
        ctx.fm->setFilter({});
        if (s.track(ctx.track).engine != EngineType::Fm) {
          ctx.flash = "track " + std::to_string(ctx.track + 1) + " isn't FM: pick an FM track (1–8) to browse onto";
          return true;
        }
        ctx.request = Context::Request::ShowInstrument;
        return true;
      }
      return false;
    }
    if (section_ == kKits) {
      if (e == Event::Return && !kits_.empty()) {
        const std::string& name = kits_[static_cast<std::size_t>(sel_)];
        if (s.track(ctx.track).engine != EngineType::Drum) {
          ctx.flash = "track " + std::to_string(ctx.track + 1) + " isn't a drum track: pick one (1–8), or ⏎ an empty one on Tracks";
          return true;
        }
        std::string error;
        if (s.loadKit(ctx.track, s.paths().kitFile(name), error)) {
          ctx.flash = "kit " + name + " loaded onto track " + std::to_string(ctx.track + 1);
          ctx.request = Context::Request::ShowInstrument;
        } else {
          ctx.flash = "kit not loaded: " + error;
        }
        return true;
      }
      return false;
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
    if (pane_ == 0) return {{"↑↓", "Section"}, {"⏎", "Open"}, {"Tab", "Pane"}, {"i", "Import FM"}, {"Esc", "Back"}};
    if (section_ == kFm) return {{"↑↓", "Move"}, {"⏎", "Browse"}, {"Tab", "Pane"}, {"i", "Import"}, {"Esc", "Back"}};
    if (section_ == kKits) return {{"↑↓", "Move"}, {"⏎", "Load onto track"}, {"Tab", "Pane"}, {"Esc", "Back"}};
    return {{"↑↓", "Move"}, {"⏎", "Load"}, {"n", "New"}, {"w", "Save"}, {"W", "Save as"}, {"i", "Import FM"}, {"Esc", "Back"}};
  }

 private:
  enum Section { kProjects = 0, kFm = 1, kKits = 2, kSections = 3 };
  enum class Action { None, Load, New };

  int rowCount(Context&) const {
    if (section_ == kKits) return static_cast<int>(kits_.size());
    return section_ == kProjects ? static_cast<int>(projects_.size()) : static_cast<int>(FmBrowse::views().size());
  }

  Element sections(Context& ctx) {
    const Session& s = ctx.session;
    const FmLibrary* lib = s.library();
    std::size_t favourites = 0;
    if (lib)
      for (const FmPatch& p : lib->patches()) favourites += p.favourite;
    auto row = [&](int index, const std::string& name, const std::string& count) {
      const bool on = section_ == index;
      Element r = hbox({text(" " + name) | flex, text(count + " ") | color(on ? palette::accent : palette::dim)});
      return on ? r | color(palette::accent) | bgcolor(palette::raised) : r;
    };
    Elements rows = {row(kProjects, "Projects", std::to_string(projects_.size())),
                     row(kFm, "FM patches", lib ? thousands(lib->patches().size()) : "—")};
    rows.push_back(hbox({text("  ★ favourites") | flex, text(thousands(favourites) + " ")}) | color(palette::dim));
    rows.push_back(row(kKits, "Drum kits", std::to_string(kits_.size())));
    rows.push_back(text(""));
    for (const char* later : {"Plaits · Phase 7", "Recordings · Phase 6"})
      rows.push_back(text(" " + std::string(later)) | color(palette::faint));
    rows.push_back(filler());
    return pane("Browse", vbox(rows), pane_ == 0);
  }

  Element middle(Context& ctx) {
    Session& s = ctx.session;
    const bool focused = pane_ == 1;
    Elements rows;
    if (section_ == kFm) {
      const FmLibrary* lib = s.library();
      rows.push_back(hbox({text("   view") | flex, text("patches ")}) | color(palette::dim));
      const auto& views = FmBrowse::views();
      // Counting means filtering the whole library once per view: only when it changed.
      if (lib && lib->generation() != countsFor_) {
        countsFor_ = lib->generation();
        counts_.assign(views.size(), 0);
        for (std::size_t i = 0; i < views.size(); ++i)
          counts_[i] = lib->filter({}, views[i].category, views[i].kind == FmBrowse::View::Kind::Favourites,
                                   views[i].kind == FmBrowse::View::Kind::Recent)
                           .size();
      }
      for (int i = 0; i < static_cast<int>(views.size()); ++i) {
        const auto& v = views[static_cast<std::size_t>(i)];
        std::size_t n = i < static_cast<int>(counts_.size()) ? counts_[static_cast<std::size_t>(i)] : 0;
        if (v.kind == FmBrowse::View::Kind::Programs) n = s.programs().size();
        const bool on = i == sel_;
        Element r = hbox({text("   " + v.label) | flex, text(thousands(n) + " ") | color(on ? palette::bg : palette::dim)});
        rows.push_back(on ? r | color(palette::bg) | bgcolor(focused ? palette::accent : palette::raised) : r);
      }
      rows.push_back(filler());
      rows.push_back(text(" categories come from patch names and are rough: tags (t) are better") | color(palette::faint));
      return pane("FM library", vbox(rows), focused);
    }

    if (section_ == kKits) {
      rows.push_back(hbox({text("   name") | flex, text("pads  ")}) | color(palette::dim));
      for (int i = 0; i < static_cast<int>(kits_.size()); ++i) {
        const std::string& name = kits_[static_cast<std::size_t>(i)];
        const bool on = i == sel_;
        int pads = 0;
        if (on || i < 40)
          for (const std::string& smp : kitFor(s, name).samples) pads += !smp.empty();
        const bool current = s.track(ctx.track).engine == EngineType::Drum && s.track(ctx.track).kit == name;
        Element row = hbox({text(current ? " ● " : "   ") | color(on ? palette::bg : palette::accent), text(name) | flex,
                            text(std::to_string(pads) + "     ") | color(on ? palette::bg : palette::dim)});
        rows.push_back(on ? row | color(palette::bg) | bgcolor(focused ? palette::accent : palette::raised) : row);
      }
      if (kits_.empty()) rows.push_back(text("   no kits yet: k on a drum track saves one") | color(palette::faint));
      rows.push_back(filler());
      rows.push_back(text(" " + abbreviateHome(s.paths().kitsDir.string())) | color(palette::faint));
      return pane("Drum kits", vbox(rows), focused);
    }

    rows.push_back(hbox({text("   name") | flex, text("tracks  ")}) | color(palette::dim));
    for (int i = 0; i < static_cast<int>(projects_.size()); ++i) {
      const std::string& name = projects_[static_cast<std::size_t>(i)];
      const bool current = name == s.name();
      const bool on = i == sel_;
      int used = 0;
      if (on || i < 40) {
        const auto summary = summaryFor(s, name);
        for (const auto& t : summary.tracks) used += t.engine != EngineType::None;
      }
      Element row = hbox({text(current ? " ● " : "   ") | color(on ? palette::bg : palette::accent), text(name) | flex,
                          text(s.hasRecovery(name) ? "autosave " : "") | color(on ? palette::bg : palette::record),
                          text(std::to_string(used) + "       ") | color(on ? palette::bg : palette::dim)});
      rows.push_back(on ? row | color(palette::bg) | bgcolor(focused ? palette::accent : palette::raised) : row);
    }
    if (projects_.empty()) rows.push_back(text("   no saved projects yet: w saves this one") | color(palette::faint));
    rows.push_back(filler());
    rows.push_back(text(" " + s.paths().projectsDir.string()) | color(palette::faint));
    return pane("Projects", vbox(rows), focused);
  }

  Element detail(Context& ctx) {
    Session& s = ctx.session;
    Elements info;
    if (section_ == kFm) {
      const FmLibrary* lib = s.library();
      if (!lib) {
        info.push_back(text("The FM library couldn't be opened.") | color(palette::record));
      } else if (const auto last = lib->lastImport()) {
        info.push_back(text("last FM import") | color(palette::dim));
        info.push_back(text(thousands(static_cast<std::size_t>(last->banks)) + " banks → " +
                            thousands(static_cast<std::size_t>(last->voices)) + " voices → " +
                            thousands(static_cast<std::size_t>(last->added)) + " new") |
                       color(palette::text));
        info.push_back(text(fit(last->folder, 60)) | color(palette::faint));
        info.push_back(text(when(last->time)) | color(palette::faint));
        if (last->corrupt) info.push_back(text(std::to_string(last->corrupt) + " corrupt banks skipped · see import log") | color(palette::record));
        info.push_back(text(""));
        info.push_back(text(thousands(lib->patches().size()) + " unique patches · duplicates hidden") | color(palette::value));
      } else {
        info.push_back(text("Nothing imported yet.") | color(palette::dim));
      }
      if (ctx.importTotal > 0)
        info.push_back(text("importing… " + std::to_string(ctx.importDone) + " of " + std::to_string(ctx.importTotal)) |
                       color(palette::accent));
      else if (!ctx.importMessage.empty())
        info.push_back(paragraph(ctx.importMessage) |
                       color(ctx.importMessage.rfind("imported", 0) == 0 ? palette::green : palette::record));
      info.push_back(text(""));
      info.push_back(paragraph("i imports a folder of .syx banks, sub-folders included. Importing again is safe: "
                               "duplicates are merged and only new voices are added.") |
                     color(palette::dim));
      info.push_back(text(""));
      info.push_back(text(fit("log: " + s.paths().importLog().string(), 60)) | color(palette::faint));
      return pane("Import", vbox(info), false);
    }

    if (section_ == kKits) {
      if (kits_.empty()) {
        info.push_back(paragraph("A kit is 16 pads: each sample (where it is on disk) and every pad setting. "
                                 "k on a drum track's Instrument screen saves one.") |
                       color(palette::dim));
        return pane("Kit", vbox(info), false);
      }
      const std::string& name = kits_[static_cast<std::size_t>(sel_)];
      const auto& kit = kitFor(s, name);
      info.push_back(text(name) | bold | color(palette::lilac));
      info.push_back(text(""));
      for (int p = 0; p < DrumEngine::kPads; ++p) {
        const std::string& smp = kit.samples[static_cast<std::size_t>(p)];
        std::error_code ec;
        const bool missing = !smp.empty() && !std::filesystem::exists(expandHome(smp), ec);
        char num[8];
        std::snprintf(num, sizeof num, " %2d  ", p + 1);
        info.push_back(hbox({text(num) | color(palette::dim),
                             text(smp.empty() ? "—" : std::filesystem::path(smp).stem().string() + (missing ? "  missing" : "")) |
                                 color(smp.empty() ? palette::faint : missing ? palette::record : palette::text)}));
      }
      info.push_back(text(""));
      info.push_back(paragraph("⏎ loads it onto the selected drum track (all 16 pads)") | color(palette::dim));
      return pane("Kit", vbox(info), false);
    }

    const std::string name = projects_.empty() ? s.name() : projects_[static_cast<std::size_t>(sel_)];
    info.push_back(text(name) | bold | color(palette::value));
    if (name == s.name())
      info.push_back(text(s.dirty() ? "● open · unsaved changes" : "● open · saved") | color(s.dirty() ? palette::accent : palette::dim));
    const auto summary = summaryFor(s, name);
    if (summary.ok) {
      if (!summary.savedAt.empty()) info.push_back(text("saved " + summary.savedAt.substr(0, 16)) | color(palette::faint));
      info.push_back(text(""));
      info.push_back(text("tracks") | color(palette::dim));
      for (int t = 0; t < kTrackCount; ++t) {
        const auto& tr = summary.tracks[static_cast<std::size_t>(t)];
        info.push_back(hbox({text(" " + std::to_string(t + 1) + "  "), text(tr.engine == EngineType::None ? "—   " : engineLabel(tr.engine)) |
                                                                             size(WIDTH, EQUAL, 6) | color(engineColor(tr.engine)),
                             text(tr.engine == EngineType::None ? "" : (tr.sound.empty() ? "INIT VOICE" : tr.sound)) | color(palette::text)}));
      }
      if (summary.programs) info.push_back(text("program list " + std::to_string(summary.programs)) | color(palette::dim));
    }
    info.push_back(text(""));
    info.push_back(paragraph("⏎ loads it · n starts a new one · w saves · W saves under a new name") | color(palette::dim));
    if (pending_ != Action::None)
      info.push_back(paragraph(std::string("unsaved changes: ") + (pending_ == Action::New ? "n" : "⏎") +
                               " again to discard them, Esc to cancel") |
                     color(palette::record));
    return pane("Project", vbox(info), false);
  }

  // Reading every project file each frame would be wasteful; cached, and
  // dropped about once a second in render().
  const Session::ProjectSummary& summaryFor(Session& s, const std::string& name) {
    auto it = summaries_.find(name);
    if (it == summaries_.end()) it = summaries_.emplace(name, s.projectSummary(name)).first;
    return it->second;
  }

  const Session::KitSummary& kitFor(Session& s, const std::string& name) {
    auto it = kitSummaries_.find(name);
    if (it == kitSummaries_.end()) it = kitSummaries_.emplace(name, s.kitSummary(name)).first;
    return it->second;
  }

  std::vector<std::string> projects_, kits_;
  std::map<std::string, Session::ProjectSummary> summaries_;
  std::map<std::string, Session::KitSummary> kitSummaries_;
  unsigned frames_ = 0;
  std::vector<std::size_t> counts_;
  uint64_t countsFor_ = ~0ull;
  int section_ = kProjects;
  int pane_ = 1;
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

std::unique_ptr<Screen> makeLibraryScreen() { return std::make_unique<LibraryScreen>(); }
std::unique_ptr<Screen> makePlaceholderScreen(std::string title, std::string message) {
  return std::make_unique<PlaceholderScreen>(std::move(title), std::move(message));
}

}  // namespace ion::ui
