#pragma once

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

// What every screen can see and do.
struct Context {
  Context(Session& s, Module& m, PipeWireOutput* a, AlsaSeqInput* mi) : session(s), module(m), audio(a), midi(mi) {}
  Session& session;
  Module& module;
  PipeWireOutput* audio = nullptr;  // null in tests and offline
  AlsaSeqInput* midi = nullptr;
  int track = 0;        // selected track, 0-7
  int width = 130;      // terminal size this frame
  int height = 36;
  bool piano = false;   // piano mode on (pane borders change colour)

  bool wide() const { return width >= 110; }
  bool narrow() const { return width < 80; }

  // Requests from screens to the app.
  enum class Request { None, ChooseEngine, ShowMixer, ShowInstrument, Prompt };
  Request request = Request::None;
  std::string flash;  // a one-line message for the status line

  // For Request::Prompt: a one-line text entry.
  std::string promptTitle;
  std::string promptInitial;
  std::function<void(Context&, const std::string&)> promptDone;
};

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
};

std::unique_ptr<Screen> makeTracksScreen();
std::unique_ptr<Screen> makeMixerScreen();
std::unique_ptr<Screen> makeInstrumentScreen();
std::unique_ptr<Screen> makeSettingsScreen();
std::unique_ptr<Screen> makeLibraryScreen();
std::unique_ptr<Screen> makePlaceholderScreen(std::string title, std::string message);

}  // namespace ion::ui
