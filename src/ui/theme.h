#pragma once

// Colours (HANDOFF §8) and the drawing pieces every screen shares.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "engine/engine.h"

namespace ion::ui {

namespace palette {
inline const ftxui::Color bg = ftxui::Color::RGB(0x0F, 0x12, 0x18);
inline const ftxui::Color surface = ftxui::Color::RGB(0x14, 0x1A, 0x22);
inline const ftxui::Color raised = ftxui::Color::RGB(0x1B, 0x22, 0x2C);
inline const ftxui::Color border = ftxui::Color::RGB(0x39, 0x42, 0x4F);
inline const ftxui::Color borderSubtle = ftxui::Color::RGB(0x2A, 0x31, 0x40);
inline const ftxui::Color text = ftxui::Color::RGB(0xD8, 0xDE, 0xE9);
inline const ftxui::Color dim = ftxui::Color::RGB(0x8B, 0x95, 0xA7);
inline const ftxui::Color faint = ftxui::Color::RGB(0x5C, 0x66, 0x77);
inline const ftxui::Color accent = ftxui::Color::RGB(0xF0, 0xA0, 0x4B);
inline const ftxui::Color value = ftxui::Color::RGB(0x6C, 0xC5, 0xD9);
inline const ftxui::Color lilac = ftxui::Color::RGB(0xC3, 0xA6, 0xFF);
inline const ftxui::Color green = ftxui::Color::RGB(0x9B, 0xD4, 0x8A);
inline const ftxui::Color record = ftxui::Color::RGB(0xFF, 0x7A, 0x7A);
inline const ftxui::Color acid = ftxui::Color::RGB(0xE6, 0xD8, 0x5C);   // ACID engine
inline const ftxui::Color chip = ftxui::Color::RGB(0xF2, 0x8F, 0xB8);   // CHIP engine
inline const ftxui::Color selectedOff = ftxui::Color::RGB(0x8A, 0x5A, 0x26);  // unlit bar on a selected row
}  // namespace palette

ftxui::Color engineColor(EngineType type);

struct Hint {
  std::string key;
  std::string label;
};
using Hints = std::vector<Hint>;

// A box-drawn pane with its title set into the top border. `borderColor`
// marks focus (accent) or piano mode (value).
ftxui::Element pane(const std::string& title, ftxui::Element content, ftxui::Color borderColor);
ftxui::Element pane(const std::string& title, ftxui::Element content, bool focused);

// What a meter shows: RMS (the solid bar), peak (shaded on from the RMS) and
// a held peak marker. All linear.
struct MeterReading {
  float rms = 0, peak = 0, hold = 0;
};

// Peak hold for one meter: holds the highest peak for 1.5 s, then falls.
// Lives in the UI (it's about what you see, not the audio).
class PeakHold {
 public:
  MeterReading update(float rms, float peak);

 private:
  float hold_ = 0;
  double heldAt_ = 0;
};

// Horizontal level meter over −48…+6 dB with eighth-cell resolution.
ftxui::Element levelBar(float linear, int width, bool selected = false);
ftxui::Element levelBar(const MeterReading& m, int width, bool selected = false);
// A value bar for a normalized 0…1 value.
// `marker` (0…1, or −1 for none) marks where modulation has the value now (D-58).
ftxui::Element valueBar(float normalized, int width, ftxui::Color fill, bool selected = false, float marker = -1);
// Vertical meter column (`height` rows), −48…+6 dB: RMS solid, peak shaded,
// held peak as a marker.
ftxui::Element verticalMeter(const MeterReading& m, int height);

// "Key Label" blocks for the bottom hint bar; hints that don't fit in `width`
// are dropped from the end (the full list is in the ? overlay).
// `decorate`, if given, wraps each block (the app makes them clickable).
ftxui::Element hintBar(const Hints& hints, int width = 1000,
                       const std::function<ftxui::Element(ftxui::Element, const Hint&)>& decorate = {});

// One labelled value row; `focused` highlights it like the mockups.
ftxui::Element paramRow(const std::string& label, const std::string& value, bool focused, int labelWidth = 14,
                        ftxui::Element extra = nullptr);

float toDb(float linear);
std::string dbText(float linear);   // "−8.2" / "−∞"
std::string fit(const std::string& s, int width);  // truncate with … (UTF-8 aware)

}  // namespace ion::ui
