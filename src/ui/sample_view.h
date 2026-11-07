#pragma once

// The sample browser as a pane, shared by the Drum and Sampler screens, and a
// few ways of showing a sample.

#include <functional>
#include <string>

#include "engine/sample.h"
#include "ui/sample_browse.h"
#include "ui/screens.h"

namespace ion::ui {

// What ⏎ on a file (or a kit) does, and what the pane says about it.
struct BrowserTarget {
  std::string loadHint = "⏎ loads it";  // "⏎ onto pad 1"
  std::string kitHint = "kits are for drum tracks";
  std::function<void(Context&, const SampleBrowse::Entry&)> load;
};

// ↑ ↓ (previewing), ⏎ / → into folders, ← up, / filter; ⏎ on a file calls target.load.
bool sampleBrowserEvent(Context& ctx, const ftxui::Event& e, const BrowserTarget& target);
ftxui::Element sampleBrowserPane(Context& ctx, int width, int height, bool focused, const BrowserTarget& target);

std::string secondsText(double seconds);  // "0.42 s", "12.3 s", "1:05"
std::string rateText(unsigned rate);      // "44.1k"
// The sample's level over its length, one block character per column (dB scale).
std::string waveformText(const Sample& s, int width);

}  // namespace ion::ui
