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
#include "library/fm_library.h"
#include "midi/alsa_seq_in.h"
#include "project/session.h"
#include "ui/fm_browse.h"
#include "ui/piano.h"
#include "ui/screens.h"
#include "ui/theme.h"

namespace ion::ui {
namespace {

using namespace ftxui;
using Clock = std::chrono::steady_clock;

constexpr int kScreenCount = 7;
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
    screens_[static_cast<int>(ScreenId::Tracks)] = makeTracksScreen();
    screens_[static_cast<int>(ScreenId::Instrument)] = makeInstrumentScreen();
    screens_[static_cast<int>(ScreenId::Mixer)] = makeMixerScreen();
    screens_[static_cast<int>(ScreenId::Fx)] =
        makePlaceholderScreen("Effects", "Compressor, chorus, delay and reverb, insert slots and send buses arrive in Phase 4.");
    screens_[static_cast<int>(ScreenId::Rec)] =
        makePlaceholderScreen("Resample", "Clock-synced recording, slicing onto pads and loading into the sampler arrive in Phase 6.");
    screens_[static_cast<int>(ScreenId::Library)] = makeLibraryScreen();
    screens_[static_cast<int>(ScreenId::Settings)] = makeSettingsScreen();
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
    piano_.setOctave(3);
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
    Element body = screen().render(ctx_);
    Element frame = vbox({header(), body | flex, statusLine(), hintBar(currentHints(), width)}) | bgcolor(palette::bg) |
                    color(palette::text);
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
    if (e == Event::CtrlC) {
      quit_ = true;
      return true;
    }
    if (overlay_ != Overlay::None) return overlayEvent(e);

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
          case 'm': s.nudgeParam({static_cast<uint8_t>(ctx_.track), kMute}, 1, false); return true;
          case 's': s.nudgeParam({static_cast<uint8_t>(ctx_.track), kSolo}, 1, false); return true;
          case ' ': audition(); return true;
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
      s.sendMidi({static_cast<uint8_t>(0x80 | a.channel), kAuditionNote, 0});
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
    s.sendMidi({static_cast<uint8_t>(0x90 | ch), kAuditionNote, 100});
    auditionOffs_.push_back({Clock::now() + std::chrono::milliseconds(s.config().gateMs), ch});
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
                                             EngineType::None};
        if (e == Event::Escape) overlay_ = Overlay::None;
        else if (e == Event::ArrowUp) picker_ = (picker_ + 4) % 5;
        else if (e == Event::ArrowDown) picker_ = (picker_ + 1) % 5;
        else if (e == Event::Return) {
          const EngineType t = options[picker_];
          if (t == EngineType::Fm || t == EngineType::None) {
            s.setEngine(ctx_.track, t);
            overlay_ = Overlay::None;
            if (t == EngineType::Fm) show(ScreenId::Instrument);  // straight to the library
            flash(t == EngineType::None ? "track " + std::to_string(ctx_.track + 1) + " cleared"
                                        : "track " + std::to_string(ctx_.track + 1) + " is now FM");
          }
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
        static const char* labels[] = {"FM      DX7-compatible 6-operator", "DRUM    16-pad drum sampler  · Phase 3",
                                       "SMPL    keymapped sampler    · Phase 5", "PLTS    Plaits macro-oscillator · Phase 7",
                                       "empty   clear the track"};
        Elements rows;
        for (int i = 0; i < 5; ++i) {
          const bool available = i == 0 || i == 4;
          Element row = text(" " + std::string(labels[i]) + " ");
          if (i == picker_) row = row | color(palette::bg) | bgcolor(available ? palette::accent : palette::faint);
          else row = row | color(available ? palette::text : palette::faint);
          rows.push_back(row);
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
    Element global = section("Everywhere", {{"T I M F R L", "Tracks · Instrument · Mixer · FX · Rec · Library"},
                                            {",", "Settings"},
                                            {"1 – 8", "select track"},
                                            {"↑ ↓ ← →", "move · change the value (Shift = coarse)"},
                                            {"Tab", "switch pane"},
                                            {"⏎", "edit / open"},
                                            {"Esc", "back · cancel"},
                                            {"Space", "audition the selected track"},
                                            {"m  s", "mute · solo the selected track"},
                                            {"l", "MIDI-learn the focused value"},
                                            {"p", "panic: all notes off"},
                                            {"w  W", "save · save as"},
                                            {"`", "piano mode"},
                                            {"~", "MIDI monitor"},
                                            {"q", "quit"}});
    Element piano = section("Piano mode (`)", {{"Z S X D C…", "lower octave, from C"},
                                               {"Q 2 W 3 E…", "upper octave"},
                                               {"[  ]", "octave down / up"},
                                               {"\\", "velocity 64 / 100 / 127"},
                                               {"Esc", "leave piano mode"}});
    Elements screenKeys = {text("This screen") | bold | color(palette::accent)};
    for (const Hint& h : screen().hints(ctx_))
      screenKeys.push_back(hbox({text(h.key) | bold | color(palette::value) | size(WIDTH, EQUAL, 13), text(h.label)}));
    char gate[64];
    std::snprintf(gate, sizeof gate, "notes play for %d ms (gate mode)", ctx_.session.config().gateMs);
    Element body = vbox({hbox({global, text("    "), vbox({piano, text(gate) | color(palette::faint), text(""), vbox(screenKeys)})}),
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
    static const char* longNames[][2] = {{"T", "racks"}, {"I", "nstr"}, {"M", "ixer"}, {"F", "x"}, {"R", "ec"}, {"L", "ibrary"}};
    static const char* shortNames[][2] = {{"T", "rk"}, {"I", "ns"}, {"M", "ix"}, {"F", "x"}, {"R", "ec"}, {"L", "ib"}};
    Elements tabs;
    for (int i = 0; i < 6; ++i) {
      const bool on = static_cast<int>(current_) == i;
      const auto& n = narrow ? shortNames[i] : longNames[i];
      tabs.push_back(hbox({text(n[0]) | bold | color(on ? palette::bg : palette::accent), text(n[1]) | color(on ? palette::bg : palette::dim)}) |
                     bgcolor(on ? palette::accent : palette::bg));
      tabs.push_back(text(" "));
    }
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
    return hbox({text(" ion") | bold | color(palette::accent), text(narrow ? " " : "  "), hbox(tabs), filler(),
                 text(project) | color(palette::dim), text(s.dirty() && wide ? " ● " : (wide ? "   " : "")) | color(palette::accent),
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
  bool promptPath_ = false;
  std::string importing_;
  ImportProgress importProgress_;
  std::future<ImportReport> import_;
  int64_t monitorStart_ = 0;
  uint32_t clockTicks_ = 0;
};

}  // namespace

int run(Session& session, Module& module, const RunOptions& options) {
  auto screen = ScreenInteractive::Fullscreen();
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

std::string snapshot(Session& session, Module& module, ScreenId id, int width, int height, bool piano, bool help, int track) {
  App app(session, module, nullptr, nullptr);
  app.selectTrack(track);
  app.show(id);
  if (piano) app.setPiano(true);
  if (help) app.showHelp();
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
