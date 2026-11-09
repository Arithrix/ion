// The app shell: header, screens, status line, hint bar, overlays, global keys
// (HANDOFF §8, DECISIONS D-06) and piano mode.

#include "ui/app.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <map>
#include <filesystem>
#include <future>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "audio/pipewire_out.h"
#include "core/version.h"
#include "engine/module.h"
#include "engine/drum/drum_engine.h"
#include "engine/sampler/sampler_engine.h"
#include "library/fm_library.h"
#include "midi/alsa_seq_in.h"
#include "project/session.h"
#include "ui/fm_browse.h"
#include "ui/sample_browse.h"
#include "ui/piano.h"
#include "ui/screens.h"
#include "ui/theme.h"

namespace ion::ui {
namespace {

using namespace ftxui;
using Clock = std::chrono::steady_clock;

constexpr int kScreenCount = 8;
constexpr uint8_t kAuditionNote = 60;  // C3

std::string describeMidi(const MidiMessage& m) {
  char buf[64];
  const int ch = m.channel() + 1;
  switch (m.type()) {
    case 0x90:
      if (m.data2 == 0) std::snprintf(buf, sizeof buf, "note off  ch%02d %s", ch, noteName(m.data1).c_str());
      else std::snprintf(buf, sizeof buf, "note on   ch%02d %s vel %d", ch, noteName(m.data1).c_str(), m.data2);
      return buf;
    case 0x80: std::snprintf(buf, sizeof buf, "note off  ch%02d %s", ch, noteName(m.data1).c_str()); return buf;
    case 0xB0: std::snprintf(buf, sizeof buf, "cc        ch%02d #%d = %d", ch, m.data1, m.data2); return buf;
    case 0xC0: std::snprintf(buf, sizeof buf, "program   ch%02d %d", ch, m.data1 + 1); return buf;
    case 0xE0: std::snprintf(buf, sizeof buf, "bend      ch%02d %+d", ch, ((m.data2 << 7) | m.data1) - 8192); return buf;
    case 0xD0: std::snprintf(buf, sizeof buf, "pressure  ch%02d %d", ch, m.data1); return buf;
    default: break;
  }
  switch (m.status) {
    case 0xF8: return "clock";
    case 0xFA: return "start";
    case 0xFB: return "continue";
    case 0xFC: return "stop";
    case 0xF2: std::snprintf(buf, sizeof buf, "song position %d", m.data1 | (m.data2 << 7)); return buf;
    default: break;
  }
  std::snprintf(buf, sizeof buf, "0x%02X", m.status);
  return buf;
}

class App {
 public:
  App(Session& session, Module& module, PipeWireOutput* audio, AlsaSeqInput* midi)
      : ctx_(session, module, audio, midi),
        piano_(session.config().lowerRow, session.config().upperRow, session.config().gateMs) {
    ctx_.fm = &fm_;
    ctx_.samples = &samples_;
    screens_[static_cast<int>(ScreenId::Tracks)] = makeTracksScreen();
    screens_[static_cast<int>(ScreenId::Instrument)] = makeInstrumentScreen();
    screens_[static_cast<int>(ScreenId::Mixer)] = makeMixerScreen();
    screens_[static_cast<int>(ScreenId::Fx)] = makeFxScreen();
    screens_[static_cast<int>(ScreenId::Rec)] = makeRecScreen();
    screens_[static_cast<int>(ScreenId::Library)] = makeLibraryScreen();
    screens_[static_cast<int>(ScreenId::Settings)] = makeSettingsScreen();
    screens_[static_cast<int>(ScreenId::Mod)] = makeModScreen();
  }

  ~App() {
    if (import_.valid()) {
      importProgress_.cancel = true;
      import_.wait();
    }
  }

  void show(ScreenId id) { current_ = id; }
  void selectTrack(int t) { ctx_.track = std::clamp(t, 0, kTrackCount - 1); }
  void setPiano(bool on) {
    if (!on) releasePiano();
    // Drum tracks start at octave 1, so the Z row plays pads 1-16 (C1-D#2).
    piano_.setOctave(ctx_.session.track(ctx_.track).engine == EngineType::Drum ? 1 : 3);
    pianoOn_ = on;
  }
  void showHelp() { overlay_ = Overlay::Help; }
  void offerRecovery() { overlay_ = Overlay::Recovery; }
  void flash(const std::string& msg, int ms = 3000) {
    flash_ = msg;
    flashUntil_ = Clock::now() + std::chrono::milliseconds(ms);
  }
  bool quitRequested() const { return quit_; }

  // ---- frame ----

  Element render(int width, int height) {
    ctx_.width = width;
    ctx_.height = height;
    ctx_.piano = pianoOn_;
    ctx_.hits.clear();
    Element body = screen().render(ctx_);
    Element hints = hintBar(currentHints(), width, [this](Element e, const Hint& h) {
      const auto key = hintKey(h.key);
      return key ? ctx_.button(std::move(e), *key) : e;
    });
    Element frame = vbox({header(), body | flex, statusLine(), hints}) | bgcolor(palette::bg) | color(palette::text);
    overlayHits_ = ctx_.hits.size();
    if (overlay_ != Overlay::None) {
      // A one-cell margin keeps the overlay's border from fusing with the
      // pane borders underneath (FTXUI joins adjacent box-drawing characters).
      Element o = vbox({text(""), hbox({text(" "), overlay(), text(" ")}), text("")}) | clear_under;
      frame = dbox({frame, o | center});
    }
    return frame;
  }

  // ---- input ----

  bool onEvent(const Event& e) {
    if (e == Event::Custom) {
      tick();
      return true;
    }
    const bool handled = handleEvent(e);
    // Whatever that key or click changed is one undo step (D-54).
    ctx_.session.commitEdit();
    return handled;
  }

  // Ctrl+Z / Ctrl+Y and the header's ↶ ↷.
  void undo() {
    const std::string what = ctx_.session.undo();
    flash(what.empty() ? "nothing to undo" : "undid " + what);
  }
  void redo() {
    const std::string what = ctx_.session.redo();
    flash(what.empty() ? "nothing to redo" : "redid " + what);
  }

 private:
  bool handleEvent(const Event& e) {
    if (e == Event::CtrlC) {
      quit_ = true;
      return true;
    }
    if (e.is_mouse()) return mouseEvent(Event(e).mouse());
    if (overlay_ != Overlay::None) return overlayEvent(e);
    if (e == Event::CtrlZ) {
      undo();
      return true;
    }
    if (e == Event::CtrlY) {
      redo();
      return true;
    }

    Session& s = ctx_.session;
    if (s.learning() && e == Event::Escape) {
      s.cancelLearn();
      flash("learn cancelled");
      return true;
    }

    if (pianoOn_) {
      if (e == Event::Escape || e == Event::Character('`')) {
        setPiano(false);
        return true;
      }
      if (e == Event::Character('[')) { piano_.octaveDown(); return true; }
      if (e == Event::Character(']')) { piano_.octaveUp(); return true; }
      if (e == Event::Character('\\')) { piano_.cycleVelocity(); return true; }
      if (e.is_character()) {
        const std::string& c = e.character();
        if (c.size() == 1 && piano_.isPianoKey(c[0])) {
          if (auto n = piano_.press(c[0], Clock::now())) {
            const uint8_t ch = static_cast<uint8_t>(s.track(ctx_.track).channel);
            pianoChannel_[n->note] = ch;
            s.sendMidi({static_cast<uint8_t>(0x90 | ch), n->note, n->velocity});
          }
        }
        return true;  // letters and digits play notes, never shortcuts
      }
      return dispatchToScreen(e);  // arrows, Tab, ⏎ keep working
    }

    // A screen taking typed text (the FM filter) gets letters before they're shortcuts.
    if (screen().capturesText(ctx_) && (e.is_character() || e == Event::Backspace)) return dispatchToScreen(e);

    if (e.is_character()) {
      const std::string& c = e.character();
      if (c.size() == 1) {
        switch (c[0]) {
          case 'T': show(ScreenId::Tracks); return true;
          case 'I': show(ScreenId::Instrument); return true;
          case 'O': show(ScreenId::Mod); return true;
          case 'M': show(ScreenId::Mixer); return true;
          case 'F': show(ScreenId::Fx); return true;
          case 'R': show(ScreenId::Rec); return true;
          case 'L': show(ScreenId::Library); return true;
          case ',': show(ScreenId::Settings); return true;
          case '?': overlay_ = Overlay::Help; return true;
          case '~': overlay_ = Overlay::Monitor; return true;
          case '`': setPiano(true); return true;
          case 'p':
            s.panic();
            flash("panic: all notes off");
            return true;
          case 'q':
            if (s.dirty()) overlay_ = Overlay::Quit;
            else quit_ = true;
            return true;
          case 'w': save(); return true;
          case 'W': startSaveAs(); return true;
          case 'l': startLearn(); return true;
          case 'v': startModulate(); return true;
          case 'm':
          case 's': {
            const int scope = screen().focusedScope(ctx_).value_or(ctx_.track);
            if (scope == kMasterScope) return true;                    // the master has neither
            if (c[0] == 's' && scope >= kTrackCount) return true;      // returns are solo-safe
            s.nudgeParam({static_cast<uint8_t>(scope), c[0] == 'm' ? kMute : kSolo}, 1, false);
            return true;
          }
          case ' ':
            if (!screen().audition(ctx_)) audition();
            return true;
          default:
            if (c[0] >= '1' && c[0] <= '8') {
              ctx_.track = c[0] - '1';
              return true;
            }
        }
      }
    }
    if (dispatchToScreen(e)) return true;
    if (e == Event::Escape && current_ != ScreenId::Tracks) {
      show(ScreenId::Tracks);
      return true;
    }
    return false;
  }

  void tick() {
    Session& s = ctx_.session;
    s.tick();
    if (std::string m = s.takeMessage(); !m.empty()) flash(m);
    const auto now = Clock::now();
    piano_.setGate(s.config().gateMs);
    for (uint8_t n : piano_.expired(now)) s.sendMidi({static_cast<uint8_t>(0x80 | pianoChannel_[n]), n, 0});
    std::erase_if(auditionOffs_, [&](const Audition& a) {
      if (a.off > now) return false;
      s.sendMidi({static_cast<uint8_t>(0x80 | a.channel), a.note, 0});
      return true;
    });
    if (ctx_.midi) {
      while (auto m = ctx_.midi->monitor().pop()) {
        if (m->msg.status == 0xF8) {
          ++clockTicks_;
          continue;
        }
        if (!monitorStart_) monitorStart_ = m->timeNs;
        monitor_.push_back({static_cast<double>(m->timeNs - monitorStart_) / 1e9, ctx_.midi->clientName(m->client), m->msg});
        if (monitor_.size() > 20) monitor_.pop_front();
      }
    }
    if (!ctx_.flash.empty()) flash(std::exchange(ctx_.flash, {}));
    pollImport();
  }

 private:
  // ---- mouse (D-55) ----

  // The smallest target under (x, y): a row wins over the pane it's in. With
  // an overlay up, only the overlay's own.
  const Hit* hitAt(int x, int y) const {
    const Hit* best = nullptr;
    long bestArea = 0;
    const std::size_t from = overlay_ != Overlay::None ? std::min(overlayHits_, ctx_.hits.size()) : 0;
    for (std::size_t i = from; i < ctx_.hits.size(); ++i) {
      const Hit& h = ctx_.hits[i];
      if (!h.box.Contain(x, y)) continue;
      const long area = static_cast<long>(h.box.x_max - h.box.x_min + 1) * (h.box.y_max - h.box.y_min + 1);
      if (!best || area <= bestArea) {
        best = &h;
        bestArea = area;
      }
    }
    return best;
  }

  bool mouseEvent(const Mouse& m) {
    // A drag in progress follows the pointer wherever it goes.
    if (drag_) {
      if (m.button == Mouse::Left && m.motion == Mouse::Moved) {
        const int cells = drag_->vertical ? drag_->origin - m.y : m.x - drag_->origin;
        const Event more = drag_->vertical ? (m.shift ? kShiftUp : Event::ArrowUp) : (m.shift ? kShiftRight : Event::ArrowRight);
        const Event less = drag_->vertical ? (m.shift ? kShiftDown : Event::ArrowDown) : (m.shift ? kShiftLeft : Event::ArrowLeft);
        while (drag_->steps < cells) {
          ++drag_->steps;
          handleEvent(more);
        }
        while (drag_->steps > cells) {
          --drag_->steps;
          handleEvent(less);
        }
        return true;
      }
      if (m.motion == Mouse::Released) {
        drag_.reset();
        return true;
      }
    }
    if (m.motion == Mouse::Released || m.motion == Mouse::Moved) return false;

    const Hit* h = hitAt(m.x, m.y);
    if (!h) {
      // A click outside an overlay that's only showing something closes it.
      if (m.button == Mouse::Left && (overlay_ == Overlay::Help || overlay_ == Overlay::Monitor || overlay_ == Overlay::EnginePicker))
        overlay_ = Overlay::None;
      return false;
    }
    // Copy what's needed: handling an event can re-render nothing, but it can
    // change what the hit's functions point at.
    const Hit hit = *h;
    if (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown) {
      const bool up = m.button == Mouse::WheelUp;
      if (hit.wheel == Hit::Wheel::Value) {
        if (hit.focus) hit.focus(ctx_);
        handleEvent(up ? Event::ArrowRight : Event::ArrowLeft);
      } else if (hit.wheel == Hit::Wheel::List) {
        if (hit.pane) hit.pane(ctx_);
        dispatchOrOverlay(up ? Event::ArrowUp : Event::ArrowDown);
      }
      return true;
    }
    if (m.button != Mouse::Left) return false;

    const auto now = Clock::now();
    const bool twice = now - lastClick_ < std::chrono::milliseconds(400) && lastClickBox_.x_min == hit.box.x_min &&
                       lastClickBox_.y_min == hit.box.y_min && lastClickBox_.x_max == hit.box.x_max &&
                       lastClickBox_.y_max == hit.box.y_max;
    lastClick_ = twice ? Clock::time_point{} : now;  // a third click starts again
    lastClickBox_ = hit.box;
    if (hit.focus) hit.focus(ctx_);
    if (hit.at) hit.at(ctx_, m.x - hit.box.x_min, m.y - hit.box.y_min);
    handleRequest();
    if (hit.click) handleEvent(*hit.click);
    else if (twice && hit.open) handleEvent(*hit.open);
    if (hit.drag != Hit::Drag::None) drag_ = Drag{hit.drag == Hit::Drag::Vertical, hit.drag == Hit::Drag::Vertical ? m.y : m.x, 0};
    return true;
  }

  // Arrow keys for a list: to the overlay if one is up, else the screen.
  void dispatchOrOverlay(const Event& e) {
    if (overlay_ != Overlay::None) overlayEvent(e);
    else dispatchToScreen(e);
  }

  struct Drag {
    bool vertical;
    int origin;
    int steps;
  };
  std::optional<Drag> drag_;
  Clock::time_point lastClick_{};
  Box lastClickBox_{};
  std::size_t overlayHits_ = 0;

  enum class Overlay { None, Help, Monitor, EnginePicker, Prompt, Quit, Recovery };

  Screen& screen() { return *screens_[static_cast<int>(current_)]; }

  bool dispatchToScreen(const Event& e) {
    const bool handled = screen().onEvent(ctx_, e);
    handleRequest();
    return handled;
  }

  void handleRequest() {
    switch (std::exchange(ctx_.request, Context::Request::None)) {
      case Context::Request::None: break;
      case Context::Request::ChooseEngine:
        overlay_ = Overlay::EnginePicker;
        picker_ = 0;
        break;
      case Context::Request::ShowMixer: show(ScreenId::Mixer); break;
      case Context::Request::ShowInstrument: show(ScreenId::Instrument); break;
      case Context::Request::ShowFx: show(ScreenId::Fx); break;
      case Context::Request::Audition: audition(); break;
      case Context::Request::Import: startImport(ctx_.importPath); break;
      case Context::Request::Prompt:
        overlay_ = Overlay::Prompt;
        promptTitle_ = ctx_.promptTitle;
        promptText_ = ctx_.promptInitial;
        promptDone_ = std::move(ctx_.promptDone);
        promptPath_ = std::exchange(ctx_.promptPath, false);
        break;
    }
    if (!ctx_.flash.empty()) flash(std::exchange(ctx_.flash, {}));
  }

  void save() {
    std::string error;
    if (ctx_.session.save(error)) flash("saved " + ctx_.session.name());
    else flash("not saved: " + error);
  }

  void startSaveAs() {
    overlay_ = Overlay::Prompt;
    promptTitle_ = "Save project as";
    promptText_ = ctx_.session.name();
    promptPath_ = false;
    promptDone_ = [this](Context& cx, const std::string& name) {
      std::string error;
      if (cx.session.saveAs(name, error)) flash("saved as " + name);
      else flash("not saved: " + error);
    };
  }

  void startLearn() {
    const auto p = screen().focusedParam(ctx_);
    if (!p) {
      flash("nothing to learn here: move to a value first");
      return;
    }
    ctx_.session.learn(*p);
  }

  // `v`: a route from LFO 1 onto the focused value (D-58).
  void startModulate() {
    Session& s = ctx_.session;
    const auto p = screen().focusedParam(ctx_);
    if (!p || !s.modulatable(*p)) {
      flash(p ? "that can't be modulated (switches, choices and other tracks' values can't)" : "nothing to modulate here: move to a value first");
      return;
    }
    const int route = s.addModRoute(*p);
    if (route < 0) {
      flash("track " + std::to_string(p->scope + 1) + " has all 8 routes in use: O to change one");
      return;
    }
    const auto src = static_cast<int>(std::lround(s.param(routeRef(p->scope, route, kRouteSource))));
    char depth[16];
    std::snprintf(depth, sizeof depth, "%+.0f%%", static_cast<double>(s.param(routeRef(p->scope, route, kRouteDepth)) * 100));
    flash(std::string(modSourceName(src)) + " → " + s.paramLabel(*p) + " " + depth + " (route " + std::to_string(route + 1) + ") · O edits it");
  }

  // ---- path completion in prompts ----

  // Splits the prompt text into the folder to look in and the name typed so far.
  std::pair<std::filesystem::path, std::string> pathParts() const {
    std::string t = promptText_;
    std::string shownDir = t.substr(0, t.rfind('/') == std::string::npos ? 0 : t.rfind('/') + 1);
    std::string prefix = t.substr(shownDir.size());
    std::string dir = shownDir.empty() ? "." : shownDir;
    if (dir.rfind("~", 0) == 0) {
      const char* home = std::getenv("HOME");
      dir = std::string(home ? home : "") + dir.substr(1);
    }
    return {std::filesystem::path(dir), prefix};
  }

  // Folders and .syx/.snd files in the typed folder that start with what's typed.
  std::vector<std::string> pathMatches() const {
    const auto [dir, prefix] = pathParts();
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind(prefix, 0) != 0 || (prefix.empty() && name[0] == '.')) continue;
      std::string ext = entry.path().extension().string();
      for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (entry.is_directory(ec)) out.push_back(name + "/");
      else if (ext == ".syx" || ext == ".snd") out.push_back(name);
      if (out.size() > 200) break;
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  void completePath() {
    const auto matches = pathMatches();
    if (matches.empty()) return;
    // The longest prefix all matches share.
    std::string common = matches.front();
    for (const std::string& m : matches) {
      std::size_t n = 0;
      while (n < common.size() && n < m.size() && common[n] == m[n]) ++n;
      common.resize(n);
    }
    const std::string typedPrefix = pathParts().second;
    if (common.size() > typedPrefix.size()) promptText_ += common.substr(typedPrefix.size());
  }

  // ---- FM library import, on a worker thread so the UI (and audio) carry on ----

  void startImport(const std::string& path) {
    if (import_.valid()) return;
    Session& s = ctx_.session;
    if (!s.library()) {
      flash("the FM library isn't open");
      return;
    }
    s.config().importFolder = path;
    std::string error;
    s.config().save(s.paths().configFile, error);
    importProgress_.filesDone = 0;
    importProgress_.filesTotal = 0;
    importProgress_.cancel = false;
    const Paths paths = s.paths();
    import_ = std::async(std::launch::async, [this, paths, path] {
      return FmLibrary::import(paths.libraryFile(), path, paths.importLog(), &importProgress_);
    });
    ctx_.importTotal = 1;  // "running" until the worker has counted the files
    importing_ = path;
    flash("importing " + path + " …", 15000);
  }

  void pollImport() {
    if (!import_.valid()) return;
    ctx_.importDone = importProgress_.filesDone;
    ctx_.importTotal = std::max(1, importProgress_.filesTotal.load());
    if (import_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const ImportReport r = import_.get();
    ctx_.importTotal = ctx_.importDone = 0;
    Session& s = ctx_.session;
    std::string error;
    if (s.library()) s.library()->reload(error);
    // Results stay up for a while, and on the Library screen until the next import.
    std::string msg;
    if (!r.error.empty()) {
      msg = "import failed: " + r.error;
    } else if (r.files == 0) {
      msg = "no .syx or .snd files in " + importing_ + " or below it";
    } else {
      msg = "imported " + std::to_string(r.files) + " files · " + std::to_string(r.banks) + " banks · " + std::to_string(r.voices) +
            " voices · " + std::to_string(r.added) + " new";
      if (r.corrupt || r.unreadable) msg += " · " + std::to_string(r.corrupt + r.unreadable) + " skipped (see import log)";
    }
    ctx_.importMessage = msg;
    flash(msg, 15000);
  }

  void audition() {
    Session& s = ctx_.session;
    if (s.track(ctx_.track).engine == EngineType::None) {
      flash("track " + std::to_string(ctx_.track + 1) + " is empty");
      return;
    }
    const auto ch = static_cast<uint8_t>(s.track(ctx_.track).channel);
    // A drum track plays its selected pad, a sampler its root note; anything else plays C3.
    uint8_t note = kAuditionNote;
    if (s.track(ctx_.track).engine == EngineType::Drum) {
      const int pad = ctx_.pad[static_cast<std::size_t>(ctx_.track)];
      note = static_cast<uint8_t>(std::lround(s.param(engineParamRef(ctx_.track, DrumEngine::param(pad, DrumEngine::kNote)))));
    } else if (s.track(ctx_.track).engine == EngineType::Sampler) {
      note = static_cast<uint8_t>(std::lround(s.param(engineParamRef(ctx_.track, SamplerEngine::kRoot))));  // as recorded
    }
    s.sendMidi({static_cast<uint8_t>(0x90 | ch), note, 100});
    auditionOffs_.push_back({Clock::now() + std::chrono::milliseconds(s.config().gateMs), ch, note});
  }

  void releasePiano() {
    for (uint8_t n : piano_.releaseAll())
      ctx_.session.sendMidi({static_cast<uint8_t>(0x80 | pianoChannel_[n]), n, 0});
  }

  // ---- overlays ----

  bool overlayEvent(const Event& e) {
    Session& s = ctx_.session;
    switch (overlay_) {
      case Overlay::Help:
      case Overlay::Monitor:
        if (e == Event::Escape || e == Event::Character('?') || e == Event::Character('~') || e == Event::Character('q'))
          overlay_ = Overlay::None;
        return true;
      case Overlay::EnginePicker: {
        static const EngineType options[] = {EngineType::Fm, EngineType::Drum, EngineType::Sampler, EngineType::Plaits,
                                             EngineType::Acid, EngineType::None};
        constexpr int kOptions = static_cast<int>(sizeof options / sizeof options[0]);
        if (e == Event::Escape) overlay_ = Overlay::None;
        else if (e == Event::ArrowUp) picker_ = (picker_ + kOptions - 1) % kOptions;
        else if (e == Event::ArrowDown) picker_ = (picker_ + 1) % kOptions;
        else if (e == Event::Return) {
          const EngineType t = options[picker_];
          s.setEngine(ctx_.track, t);
          overlay_ = Overlay::None;
          if (t != EngineType::None) show(ScreenId::Instrument);  // straight to the library / the pads / the models
          flash(t == EngineType::None ? "track " + std::to_string(ctx_.track + 1) + " cleared"
                                      : "track " + std::to_string(ctx_.track + 1) + " is now " + engineLabel(t));
        }
        return true;
      }
      case Overlay::Prompt:
        if (e == Event::Escape) overlay_ = Overlay::None;
        else if (e == Event::Return) {
          overlay_ = Overlay::None;
          if (promptDone_) promptDone_(ctx_, promptText_);
          handleRequest();
        } else if (e == Event::Tab && promptPath_) {
          completePath();
        } else if (e == Event::Backspace) {
          while (!promptText_.empty() && (static_cast<unsigned char>(promptText_.back()) & 0xC0) == 0x80) promptText_.pop_back();
          if (!promptText_.empty()) promptText_.pop_back();
        } else if (e.is_character() && promptText_.size() < 64) {
          promptText_ += e.character();
        }
        return true;
      case Overlay::Quit:
        if (e == Event::Escape) overlay_ = Overlay::None;
        else if (e == Event::Character('w')) {
          std::string error;
          if (s.save(error)) quit_ = true;
          else {
            overlay_ = Overlay::None;
            flash("not saved: " + error);
          }
        } else if (e == Event::Character('q')) {
          quit_ = true;  // main() writes the autosave, so nothing is lost
        }
        return true;
      case Overlay::Recovery:
        if (e == Event::Character('r') || e == Event::Character('y')) {
          std::string error;
          if (s.recover(s.name(), error)) flash("recovered unsaved changes; w saves them");
          else flash("can't recover: " + error);
          overlay_ = Overlay::None;
        } else if (e == Event::Character('d') || e == Event::Character('n')) {
          s.discardRecovery();
          flash("autosave discarded");
          overlay_ = Overlay::None;
        } else if (e == Event::Escape) {
          overlay_ = Overlay::None;
        }
        return true;
      case Overlay::None: break;
    }
    return false;
  }

  Element overlay() {
    switch (overlay_) {
      case Overlay::Help: return helpOverlay();
      case Overlay::Monitor: return monitorOverlay();
      case Overlay::EnginePicker: {
        static const char* labels[] = {"FM      DX7-compatible 6-operator", "DRUM    16-pad drum sampler",
                                       "SMPL    keymapped sampler", "PLTS    Plaits: 24 models",
                                       "ACID    303-style bass",
                                       "empty   clear the track"};
        Elements rows;
        for (int i = 0; i < static_cast<int>(sizeof labels / sizeof labels[0]); ++i) {
          const bool available = true;
          Element row = text(" " + std::string(labels[i]) + " ");
          if (i == picker_) row = row | color(palette::bg) | bgcolor(available ? palette::accent : palette::faint);
          else row = row | color(available ? palette::text : palette::faint);
          rows.push_back(ctx_.item(row, [this, i](Context&) { picker_ = i; }));
        }
        return pane("Engine for track " + std::to_string(ctx_.track + 1), vbox(rows), true);
      }
      case Overlay::Prompt: {
        Elements rows = {hbox({text(promptText_) | color(palette::text), text(" ") | bgcolor(palette::accent)})};
        if (promptPath_) {
          // What Tab would complete to: the folders (and .syx files) here.
          const auto matches = pathMatches();
          for (std::size_t i = 0; i < std::min<std::size_t>(matches.size(), 8); ++i)
            rows.push_back(text("  " + matches[i]) | color(palette::dim));
          if (matches.size() > 8) rows.push_back(text("  … " + std::to_string(matches.size() - 8) + " more") | color(palette::faint));
        }
        rows.push_back(text(promptPath_ ? "Tab completes · ⏎ ok · Esc cancel" : "⏎ ok · Esc cancel") | color(palette::faint));
        return pane(promptTitle_, vbox(rows) | size(WIDTH, GREATER_THAN, 40), true);
      }
      case Overlay::Quit:
        return pane("Unsaved changes",
                    vbox({text(ctx_.session.name() + " has unsaved changes."),
                          text("w  save and quit") | color(palette::value),
                          text("q  quit anyway (they're kept as an autosave you can recover)") | color(palette::dim),
                          text("Esc  stay") | color(palette::dim)}),
                    true);
      case Overlay::Recovery:
        return pane("Recover?",
                    vbox({text("\"" + ctx_.session.name() + "\" has an autosave newer than the saved project."),
                          text("ion didn't quit cleanly, or changes weren't saved."),
                          text(""),
                          text("r  recover those changes") | color(palette::value),
                          text("d  discard them and keep the saved version") | color(palette::dim),
                          text("Esc  decide later (the autosave is kept)") | color(palette::dim)}),
                    true);
      case Overlay::None: break;
    }
    return text("");
  }

  Element helpOverlay() {
    auto section = [](const std::string& title, const std::vector<std::pair<std::string, std::string>>& keys) {
      Elements rows = {text(title) | bold | color(palette::accent)};
      for (const auto& [k, label] : keys)
        rows.push_back(hbox({text(k) | bold | color(palette::value) | size(WIDTH, EQUAL, 13), text(label) | color(palette::text)}));
      return vbox(rows);
    };
    Element global = section("Everywhere", {{"T I O M F R L", "Tracks · Instr · Mod · Mixer · FX · Rec · Library"},
                                            {",", "Settings"},
                                            {"1 – 8", "select track"},
                                            {"↑ ↓ ← →", "move · change the value (Shift = coarse)"},
                                            {"Tab", "switch pane"},
                                            {"⏎", "edit / open"},
                                            {"Esc", "back · cancel"},
                                            {"Space", "audition the selected track (or pad)"},
                                            {"m  s", "mute · solo the selected track"},
                                            {"l", "MIDI-learn the focused value"},
                                            {"v", "modulate the focused value (LFO 1)"},
                                            {"p", "panic: all notes off"},
                                            {"^Z  ^Y", "undo · redo (or click ↶ ↷)"},
                                            {"w  W", "save · save as"},
                                            {"`", "piano mode"},
                                            {"~", "MIDI monitor"},
                                            {"q", "quit"}});
    Element piano = section("Piano mode (`)", {{"Z S X D C…", "lower octave, from C"},
                                               {"Q 2 W 3 E…", "upper octave"},
                                               {"[  ]", "octave down / up"},
                                               {"\\", "velocity 64 / 100 / 127"},
                                               {"drum track", "starts at octave 1: Z row = pads 1–16"},
                                               {"Esc", "leave piano mode"}});
    Elements screenKeys = {text("This screen") | bold | color(palette::accent)};
    for (const Hint& h : screen().hints(ctx_))
      screenKeys.push_back(hbox({text(h.key) | bold | color(palette::value) | size(WIDTH, EQUAL, 13), text(h.label)}));
    char gate[64];
    std::snprintf(gate, sizeof gate, "notes play for %d ms (gate mode)", ctx_.session.config().gateMs);
    Element mouse = section("Mouse", {{"click", "select · tabs, hints, ↶ ↷"},
                                      {"double-click", "open (as ⏎)"},
                                      {"wheel", "scroll a list · change a value"},
                                      {"drag", "change a value"}});
    Element body = vbox({hbox({global, text("    "),
                               vbox({piano, text(gate) | color(palette::faint), text(""), mouse, text(""), vbox(screenKeys)})}),
                         text(""), text(versionLine()) | color(palette::dim),
                         text("Esc or ? closes") | color(palette::faint)});
    return pane("Help", body, true);
  }

  Element monitorOverlay() {
    const ModuleStatus& st = ctx_.module.status();
    Elements rows;
    char clock[96];
    if (st.clockPresent.load())
      std::snprintf(clock, sizeof clock, "%s %.1f BPM · bar %d beat %d · %u ticks", st.running.load() ? "▶" : "■",
                    st.bpm.load(), st.bar.load() + 1, st.beat.load() + 1, clockTicks_);
    else
      std::snprintf(clock, sizeof clock, "no clock · %u ticks received", clockTicks_);
    rows.push_back(text(clock) | color(palette::value));
    rows.push_back(separatorLight() | color(palette::borderSubtle));
    rows.push_back(hbox({text("time     ") , text("source            "), text("message")}) | color(palette::dim));
    if (monitor_.empty()) rows.push_back(text(ctx_.midi ? "waiting for MIDI…" : "MIDI input is off") | color(palette::faint));
    for (const MonitorLine& m : monitor_) {
      char t[16];
      std::snprintf(t, sizeof t, "%8.3f ", m.seconds);
      rows.push_back(hbox({text(t) | color(palette::dim), text(fit(m.source.empty() ? "?" : m.source, 17)) | size(WIDTH, EQUAL, 18),
                           text(describeMidi(m.msg))}));
    }
    rows.push_back(text(""));
    rows.push_back(text("clock ticks are counted, not listed · piano notes don't appear here · Esc closes") | color(palette::faint));
    return pane("MIDI monitor", vbox(rows) | size(WIDTH, GREATER_THAN, 64), true);
  }

  // ---- chrome ----

  Element header() {
    const ModuleStatus& st = ctx_.module.status();
    const bool wide = ctx_.wide(), narrow = ctx_.narrow();
    // Each tab: its name split around its key (Mod's key is the O inside it).
    struct Tab {
      ScreenId id;
      char key;
      const char *before, *keyText, *after, *shortAfter;
    };
    static const Tab kTabs[] = {{ScreenId::Tracks, 'T', "", "T", "racks", "rk"}, {ScreenId::Instrument, 'I', "", "I", "nstr", "ns"},
                                {ScreenId::Mod, 'O', "M", "o", "d", "d"},         {ScreenId::Mixer, 'M', "", "M", "ixer", "ix"},
                                {ScreenId::Fx, 'F', "", "F", "x", "x"},           {ScreenId::Rec, 'R', "", "R", "ec", "ec"},
                                {ScreenId::Library, 'L', "", "L", "ibrary", "ib"}};
    Elements tabs;
    for (const Tab& tb : kTabs) {
      const bool on = current_ == tb.id;
      const Color dimC = on ? palette::bg : palette::dim;
      tabs.push_back(ctx_.button(hbox({text(tb.before) | color(dimC), text(tb.keyText) | bold | color(on ? palette::bg : palette::accent),
                                       text(narrow ? tb.shortAfter : tb.after) | color(dimC)}) |
                                     bgcolor(on ? palette::accent : palette::bg),
                                 Event::Character(tb.key)));
      tabs.push_back(text(" "));
    }
    // Undo and redo, for the mouse (Ctrl+Z, Ctrl+Y on the keyboard).
    const Session& ss = ctx_.session;
    Element undoButton = ctx_.button(text(wide ? "↶ undo" : "↶") | color(ss.canUndo() ? palette::text : palette::faint), Event::CtrlZ);
    Element redoButton = ctx_.button(text(wide ? "↷ redo" : "↷") | color(ss.canRedo() ? palette::text : palette::faint), Event::CtrlY);
    char clock[48];
    if (st.clockPresent.load()) std::snprintf(clock, sizeof clock, "%s %.1f%s", st.running.load() ? "▶" : "■", st.bpm.load(), wide ? " ext clock" : "");
    else std::snprintf(clock, sizeof clock, "%s", narrow ? "" : "no clock");
    char cpu[24];
    std::snprintf(cpu, sizeof cpu, "%s%d%%", narrow ? "" : "cpu ", static_cast<int>(std::lround(st.dspLoad.load() * 100)));
    std::string audio = "no audio";
    if (ctx_.audio) {
      audio = (wide ? "48k · " : "") + std::to_string(st.callbackFrames.load()) + (wide ? " · pipewire" : "");
      if (!ctx_.audio->realtime()) audio += narrow ? " !RT" : " · no RT";
      if (const uint32_t d = ctx_.audio->dropouts()) audio += " · " + std::to_string(d) + " drop";
    }
    const Session& s = ctx_.session;
    std::string project = wide ? fit(s.name(), 24) : "";
    // A take armed or running shows on every screen.
    std::string rec;
    if (const Take* take = s.take()) {
      const Session::RecState rs = s.recState();
      if (rs == Session::RecState::Armed) rec = narrow ? "◌" : "◌ ARMED";
      else if (rs == Session::RecState::Recording) {
        rec = "● REC";
        if (take->synced && !narrow) {
          const int bar = std::max(0, take->tickCount.load() - 1) / 96 + 1;
          rec += " " + std::to_string(std::min(bar, take->targetBars.load()));
          if (take->bars > 0) rec += "/" + std::to_string(take->targetBars.load());
        }
      }
      if (!rec.empty()) rec += narrow ? " " : "   ";
    }
    return hbox({text(" ion") | bold | color(palette::accent), text(narrow ? " " : "  "), hbox(tabs), text(narrow ? "" : " "), undoButton,
                 text(" "), redoButton, filler(),
                 text(project) | color(palette::dim), text(s.dirty() && wide ? " ● " : (wide ? "   " : "")) | color(palette::accent),
                 text(rec) | bold | color(palette::record),
                 text(clock) | color(st.clockPresent.load() ? palette::text : palette::faint), text(narrow ? " " : "   "),
                 text(cpu) | color(palette::text), text(narrow ? " " : "   "),
                 text(audio + " ") | color(ctx_.audio && !ctx_.audio->realtime() ? palette::record : palette::dim)});
  }

  Element statusLine() {
    Session& s = ctx_.session;
    const ModuleStatus& st = ctx_.module.status();
    if (s.learning())
      return hbox({text(" ● learn ") | bold | color(palette::record),
                   text("move a control on the OXI for " + s.paramLabel(*s.learning())) | color(palette::text), filler(),
                   text("Esc cancels ") | color(palette::faint)});
    if (pianoOn_) {
      char buf[96];
      std::snprintf(buf, sizeof buf, " ♪ PIANO · oct %d · vel %d · gate %d ms · track %d", piano_.octave(), piano_.velocity(),
                    piano_.gateMs(), ctx_.track + 1);
      return hbox({text(buf) | bold | color(palette::value), filler(),
                   text(Clock::now() < flashUntil_ ? flash_ + " " : "") | color(palette::accent)});
    }
    if (Clock::now() < flashUntil_) return hbox({text(" " + flash_) | color(palette::accent), filler()});
    std::string midi = "MIDI off";
    if (ctx_.midi) {
      const auto names = ctx_.midi->connected();
      midi = names.empty() ? (ctx_.narrow() ? "no OXI found" : "no OXI found — plug it in, it connects automatically")
                           : names.front() + (names.size() > 1 ? " +" + std::to_string(names.size() - 1) : "");
    }
    const uint32_t last = st.lastMidi.load();
    const std::string lastText =
        last ? describeMidi({static_cast<uint8_t>(last & 0xFF), static_cast<uint8_t>((last >> 8) & 0xFF), static_cast<uint8_t>(last >> 16)})
             : "nothing yet";
    return hbox({text(" midi ") | color(palette::dim), text(midi) | color(palette::text),
                 text(ctx_.narrow() ? " · " : "  ·  last: ") | color(palette::dim), text(lastText) | color(palette::text), filler()});
  }

  Hints currentHints() {
    switch (overlay_) {
      case Overlay::Help:
      case Overlay::Monitor: return {{"Esc", "Close"}};
      case Overlay::EnginePicker: return {{"↑↓", "Engine"}, {"⏎", "Choose"}, {"Esc", "Cancel"}};
      case Overlay::Prompt:
        if (promptPath_) return {{"Tab", "Complete"}, {"⏎", "OK"}, {"Esc", "Cancel"}};
        return {{"⏎", "OK"}, {"Esc", "Cancel"}};
      case Overlay::Quit: return {{"w", "Save & quit"}, {"q", "Quit"}, {"Esc", "Stay"}};
      case Overlay::Recovery: return {{"r", "Recover"}, {"d", "Discard"}, {"Esc", "Later"}};
      case Overlay::None: break;
    }
    if (pianoOn_) return {{"Z…/ Q…P", "Play"}, {"[ ]", "Octave"}, {"\\", "Velocity"}, {"←→", "Value"}, {"Esc", "Leave piano"}};
    return screen().hints(ctx_);
  }

  struct Audition {
    Clock::time_point off;
    uint8_t channel;
    uint8_t note;
  };
  struct MonitorLine {
    double seconds;
    std::string source;
    MidiMessage msg;
  };

  Context ctx_;
  std::array<std::unique_ptr<Screen>, kScreenCount> screens_;
  ScreenId current_ = ScreenId::Tracks;
  Overlay overlay_ = Overlay::None;
  bool quit_ = false;
  bool pianoOn_ = false;
  Piano piano_;
  std::array<uint8_t, 128> pianoChannel_{};
  std::vector<Audition> auditionOffs_;
  std::string flash_;
  Clock::time_point flashUntil_{};
  int picker_ = 0;
  std::string promptTitle_, promptText_;
  std::function<void(Context&, const std::string&)> promptDone_;
  std::deque<MonitorLine> monitor_;
  FmBrowse fm_;
  SampleBrowse samples_;
  bool promptPath_ = false;
  std::string importing_;
  ImportProgress importProgress_;
  std::future<ImportReport> import_;
  int64_t monitorStart_ = 0;
  uint32_t clockTicks_ = 0;
};

}  // namespace

std::optional<Event> hintKey(const std::string& key) {
  if (key == "⏎") return Event::Return;
  if (key == "Esc") return Event::Escape;
  if (key == "Tab") return Event::Tab;
  if (key == "Spc" || key == "Space") return Event::Character(' ');
  if (key == "^Z") return Event::CtrlZ;
  if (key == "^Y") return Event::CtrlY;
  // One character (or one UTF-8 character that's a printable key).
  if (key.size() == 1 && key[0] > ' ' && key[0] < 127) return Event::Character(key);
  return std::nullopt;
}

int run(Session& session, Module& module, const RunOptions& options) {
  auto screen = ScreenInteractive::Fullscreen();
  screen.ForceHandleCtrlZ(false);  // Ctrl+Z is undo, not suspend
  App app(session, module, options.audio, options.midi);
  if (!options.notice.empty()) app.flash(options.notice);
  if (options.offerRecovery) app.offerRecovery();

  auto renderer = Renderer([&] {
    const auto size = Terminal::Size();
    return app.render(size.dimx, size.dimy);
  });
  auto root = CatchEvent(renderer, [&](Event e) {
    const bool handled = app.onEvent(e);
    if (app.quitRequested()) screen.Exit();
    return handled;
  });

  // ~30 fps: meters, timers, monitor.
  std::atomic<bool> running{true};
  std::thread ticker([&] {
    while (running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
      screen.PostEvent(Event::Custom);
    }
  });
  screen.Loop(root);
  running = false;
  ticker.join();
  return 0;
}

std::string snapshot(Session& session, Module& module, ScreenId id, int width, int height, bool piano, bool help, int track,
                     const std::vector<std::string>& keys) {
  App app(session, module, nullptr, nullptr);
  app.selectTrack(track);
  app.show(id);
  if (piano) app.setPiano(true);
  if (help) app.showHelp();
  static const std::map<std::string, Event> named = {
      {"Up", Event::ArrowUp},   {"Down", Event::ArrowDown}, {"Left", Event::ArrowLeft},       {"Right", Event::ArrowRight},
      {"Return", Event::Return}, {"Tab", Event::Tab},        {"TabReverse", Event::TabReverse}, {"Escape", Event::Escape},
      {"Backspace", Event::Backspace}};
  // Mouse, as "click:x,y", "dclick:x,y", "wheelup:x,y", "wheeldown:x,y", "drag:x,y,toX".
  auto mouse = [](Mouse::Button button, Mouse::Motion motion, int x, int y) {
    Mouse m;
    m.button = button;
    m.motion = motion;
    m.x = x;
    m.y = y;
    return Event::Mouse("", m);
  };
  for (const std::string& k : keys) {
    // Screens size their lists from the last frame, and mouse targets are where it drew them.
    auto screen = ftxui::Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
    Render(screen, app.render(width, height));
    if (const auto colon = k.find(':'); colon != std::string::npos && colon > 1) {
      const std::string what = k.substr(0, colon);
      int x = 0, y = 0, toX = 0;
      std::sscanf(k.c_str() + colon + 1, "%d,%d,%d", &x, &y, &toX);
      if (what == "click" || what == "dclick") {
        for (int n = 0; n < (what == "dclick" ? 2 : 1); ++n) {
          app.onEvent(mouse(Mouse::Left, Mouse::Pressed, x, y));
          app.onEvent(mouse(Mouse::Left, Mouse::Released, x, y));
        }
      } else if (what == "wheelup" || what == "wheeldown") {
        app.onEvent(mouse(what == "wheelup" ? Mouse::WheelUp : Mouse::WheelDown, Mouse::Pressed, x, y));
      } else if (what == "drag") {
        app.onEvent(mouse(Mouse::Left, Mouse::Pressed, x, y));
        app.onEvent(mouse(Mouse::Left, Mouse::Moved, toX, y));
        app.onEvent(mouse(Mouse::Left, Mouse::Released, toX, y));
      }
      continue;
    }
    if (k == "^Z" || k == "^Y") {
      app.onEvent(k == "^Z" ? Event::CtrlZ : Event::CtrlY);
      continue;
    }
    const auto it = named.find(k);
    app.onEvent(it != named.end() ? it->second : Event::Character(k));
  }
  if (!keys.empty()) session.waitForSamples(5000);
  auto screen = ftxui::Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
  Render(screen, app.render(width, height));
  std::string out;
  for (int y = 0; y < height; ++y) {
    std::string line;
    for (int x = 0; x < width; ++x) {
      const std::string& c = screen.CellAt(x, y).character;
      line += c.empty() ? " " : c;
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out += line + '\n';
  }
  return out;
}

}  // namespace ion::ui
