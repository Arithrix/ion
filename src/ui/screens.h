#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "core/params.h"
#include "ui/theme.h"

namespace ion {
class Session;
class Module;
class PipeWireOutput;
class AlsaSeqInput;
}  // namespace ion

namespace ion::ui {

class FmBrowse;
class SampleBrowse;
struct Context;

// Something the mouse can act on (DECISIONS D-55). Screens mark these as they
// draw; the app looks up what's under the pointer in the last frame drawn and
// turns the click, double-click, wheel or drag into what the keys would do,
// so every mouse action is also a key and screens keep one code path.
struct Hit {
  ftxui::Box box;
  // Click: put the cursor here (the row, the pane). May be empty.
  std::function<void(Context&)> focus;
  // Click, for targets where it matters where in the box: the cell clicked,
  // from the box's top left (a waveform). Called after `focus`.
  std::function<void(Context&, int x, int y)> at;
  // After focusing, a click presses this (a button, a tab, a hint).
  std::optional<ftxui::Event> click;
  // A double-click presses this (open, load, play).
  std::optional<ftxui::Event> open = ftxui::Event::Return;
  // The wheel: List presses ↑ ↓ (after `pane`, so it scrolls the list under
  // the pointer); Value focuses and presses → ←.
  enum class Wheel : uint8_t { None, List, Value } wheel = Wheel::List;
  std::function<void(Context&)> pane;
  // Dragging with the button held presses → ← once per cell sideways, or ↑ ↓ once per row.
  enum class Drag : uint8_t { None, Horizontal, Vertical } drag = Drag::None;
};

// What every screen can see and do.
struct Context {
  Context(Session& s, Module& m, PipeWireOutput* a, AlsaSeqInput* mi) : session(s), module(m), audio(a), midi(mi) {}
  Session& session;
  Module& module;
  PipeWireOutput* audio = nullptr;  // null in tests and offline
  AlsaSeqInput* midi = nullptr;
  FmBrowse* fm = nullptr;  // the FM library browser's state, shared by Tracks and Instrument
  SampleBrowse* samples = nullptr;  // the sample browser's state
  int track = 0;        // selected track, 0-7
  std::array<int, kTrackCount> pad{};  // selected pad on each drum track, 0-15 (Space plays it)
  int width = 130;      // terminal size this frame
  int height = 36;
  bool piano = false;   // piano mode on (pane borders change colour)

  bool wide() const { return width >= 110; }
  bool narrow() const { return width < 80; }

  // Requests from screens to the app.
  enum class Request { None, ChooseEngine, ShowMixer, ShowInstrument, ShowFx, Prompt, Audition, Import };
  Request request = Request::None;
  std::string flash;  // a one-line message for the status line

  // For Request::Import: a folder or file to add to the FM library, on a worker thread.
  std::string importPath;
  // While an import runs: files done / total (total 0 = none running).
  int importDone = 0, importTotal = 0;

  // For Request::Prompt: a one-line text entry.
  std::string promptTitle;
  std::string promptInitial;
  std::function<void(Context&, const std::string&)> promptDone;
  bool promptPath = false;  // the text is a file path: Tab completes it

  // The outcome of the last FM import, kept for the Library screen.
  std::string importMessage;

  // ---- mouse (D-55): this frame's targets, rebuilt by every render ----
  std::deque<Hit> hits;  // a deque, so each box stays put while the frame lays out
  // Marks `e` as a mouse target.
  ftxui::Element hit(ftxui::Element e, Hit h) {
    hits.push_back(std::move(h));
    return e | ftxui::reflect(hits.back().box);
  }
  // A row in a list: click selects, double-click opens, the wheel scrolls.
  ftxui::Element item(ftxui::Element e, std::function<void(Context&)> focus, std::function<void(Context&)> pane = {},
                      std::optional<ftxui::Event> open = ftxui::Event::Return) {
    Hit h;
    h.focus = std::move(focus);
    h.pane = std::move(pane);
    h.open = std::move(open);
    return hit(std::move(e), std::move(h));
  }
  // A value: click focuses it, the wheel or a drag changes it.
  ftxui::Element value(ftxui::Element e, std::function<void(Context&)> focus,
                       std::optional<ftxui::Event> open = ftxui::Event::Return) {
    Hit h;
    h.focus = std::move(focus);
    h.wheel = Hit::Wheel::Value;
    h.drag = Hit::Drag::Horizontal;
    h.open = std::move(open);
    return hit(std::move(e), std::move(h));
  }
  // A button: a click presses `key`.
  ftxui::Element button(ftxui::Element e, ftxui::Event key, std::function<void(Context&)> focus = {}) {
    Hit h;
    h.focus = std::move(focus);
    h.click = std::move(key);
    h.open.reset();
    h.wheel = Hit::Wheel::None;
    return hit(std::move(e), std::move(h));
  }
};

// The key a hint names ("⏎", "Esc", "Tab", "Spc", "x"), or nothing for the
// ones that stand for several keys ("↑↓", "1-8").
std::optional<ftxui::Event> hintKey(const std::string& key);

// Shift+Arrow: FTXUI 7 has no named events for them; they arrive as raw CSI.
inline const ftxui::Event kShiftLeft = ftxui::Event::Special("\x1B[1;2D");
inline const ftxui::Event kShiftRight = ftxui::Event::Special("\x1B[1;2C");
inline const ftxui::Event kShiftUp = ftxui::Event::Special("\x1B[1;2A");
inline const ftxui::Event kShiftDown = ftxui::Event::Special("\x1B[1;2B");

// ← → (Shift or Ctrl = coarse) as a step direction, or 0.
struct Nudge {
  int direction = 0;
  bool coarse = false;
};
Nudge nudgeOf(const ftxui::Event& e);

class Screen {
 public:
  virtual ~Screen() = default;
  virtual ftxui::Element render(Context& ctx) = 0;
  // Screen-local keys. Global keys never reach here (DECISIONS D-06).
  virtual bool onEvent(Context& ctx, const ftxui::Event& e) = 0;
  virtual Hints hints(Context& ctx) = 0;
  // The parameter `l` would learn a CC for.
  virtual std::optional<ParamRef> focusedParam(Context&) { return std::nullopt; }
  // The strip `m` and `s` act on, when it isn't the selected track (the Mixer's returns).
  virtual std::optional<int> focusedScope(Context&) { return std::nullopt; }
  // True while the screen is taking typed text (a filter): letters, digits and
  // Backspace then go to onEvent instead of being shortcuts.
  virtual bool capturesText(Context&) { return false; }
  // Space, when the screen has something of its own to play (a slice); false
  // leaves it to the global audition.
  virtual bool audition(Context&) { return false; }
};

// What a track plays, in a few words: the FM patch, the kit, "— empty —".
std::string trackSoundName(const Session& s, int t);
// A scope's EQ in a few words: "hp 40 · lo +2.0", "flat", "bypassed".
std::string eqSummary(const Session& s, int scope);
// An insert in a few words: "thr −18 · 3:1", "hall · 2.4 s", "empty".
std::string insertSummary(const Session& s, int scope, int slot);

// Asks for a folder and starts an FM library import (Instrument and Library screens).
void requestImport(Context& ctx);

std::unique_ptr<Screen> makeTracksScreen();
std::unique_ptr<Screen> makeMixerScreen();
std::unique_ptr<Screen> makeFxScreen();
std::unique_ptr<Screen> makeInstrumentScreen();
std::unique_ptr<Screen> makeDrumScreen();     // the Instrument screen for DRUM tracks
std::unique_ptr<Screen> makeSamplerScreen();  // …and for SAMPLER tracks
std::unique_ptr<Screen> makeRecScreen();       // Rec (R): resampling
std::unique_ptr<Screen> makeSettingsScreen();
std::unique_ptr<Screen> makeLibraryScreen();
std::unique_ptr<Screen> makePlaceholderScreen(std::string title, std::string message);

}  // namespace ion::ui
