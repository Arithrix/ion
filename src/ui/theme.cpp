#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ion::ui {

using namespace ftxui;

Color engineColor(EngineType type) {
  switch (type) {
    case EngineType::Drum: return palette::accent;
    case EngineType::Fm: return palette::value;
    case EngineType::Sampler: return palette::lilac;
    case EngineType::Plaits: return palette::green;
    case EngineType::None: break;
  }
  return palette::faint;
}

Element pane(const std::string& title, Element content, Color borderColor) {
  const bool lit = !(borderColor == palette::border);
  return window(text(" " + title + " ") | bold | ftxui::color(lit ? borderColor : palette::dim),
                std::move(content) | ftxui::color(palette::text)) |
         ftxui::color(borderColor);
}

Element pane(const std::string& title, Element content, bool focused) {
  return pane(title, std::move(content), focused ? palette::accent : palette::border);
}

float toDb(float linear) { return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f; }

std::string dbText(float linear) {
  const float db = toDb(linear);
  if (db < -90.0f) return "−∞";
  char buf[16];
  std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(db));
  std::string s = buf;
  if (s[0] == '-') s = "−" + s.substr(1);
  return s;
}

namespace {

std::string bars(int eighths, int width, std::string& rest) {
  static const char* partial[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
  std::string on;
  for (int i = 0; i < eighths / 8; ++i) on += "█";
  const int r = eighths % 8;
  if (r) on += partial[r];
  const int used = eighths / 8 + (r ? 1 : 0);
  rest.clear();
  for (int i = used; i < width; ++i) rest += "░";
  return on;
}

}  // namespace

Element levelBar(float linear, int width, bool selected) {
  const float db = toDb(linear);
  const float frac = std::clamp((db + 48.0f) / 54.0f, 0.0f, 1.0f);
  std::string off;
  const std::string on = bars(static_cast<int>(std::lround(frac * static_cast<float>(width * 8))), width, off);
  const Color fill = selected ? palette::bg : (db > -3.0f ? palette::accent : palette::value);
  return hbox({text(on) | ftxui::color(fill), text(off) | ftxui::color(selected ? palette::selectedOff : palette::border)});
}

Element valueBar(float n, int width, Color fill, bool selected) {
  std::string off;
  const std::string on =
      bars(static_cast<int>(std::lround(std::clamp(n, 0.0f, 1.0f) * static_cast<float>(width * 8))), width, off);
  return hbox({text(on) | ftxui::color(selected ? palette::bg : fill),
               text(off) | ftxui::color(selected ? palette::selectedOff : palette::border)});
}

Element verticalMeter(float linear, int height) {
  static const char* partial[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  const float db = toDb(linear);
  const float frac = std::clamp((db + 48.0f) / 54.0f, 0.0f, 1.0f);
  const int eighths = static_cast<int>(std::lround(frac * static_cast<float>(height * 8)));
  Elements rows;
  for (int row = height - 1; row >= 0; --row) {
    const int lit = std::clamp(eighths - row * 8, 0, 8);
    const float rowDb = -48.0f + 54.0f * static_cast<float>(row + 1) / static_cast<float>(height);
    const Color c = rowDb > 0.0f ? palette::record : (rowDb > -6.0f ? palette::accent : palette::value);
    rows.push_back(text(lit ? partial[lit] : "·") | ftxui::color(lit ? c : palette::borderSubtle));
  }
  return vbox(rows);
}

Element hintBar(const Hints& hints, int width) {
  Elements items;
  int used = 0;
  auto cells = [](const std::string& s) {
    int n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
  };
  for (const Hint& h : hints) {
    const int w = cells(h.key) + cells(h.label) + 5;
    if (used + w > width) break;
    used += w;
    items.push_back(text(" " + h.key + " ") | bold | ftxui::color(palette::text));
    items.push_back(text(" " + h.label + " ") | ftxui::color(palette::bg) | bgcolor(palette::value));
    items.push_back(text(" "));
  }
  return hbox(items);
}

Element paramRow(const std::string& label, const std::string& value, bool focused, int labelWidth, Element extra) {
  Elements row = {text(fit(label, labelWidth)) | size(WIDTH, EQUAL, labelWidth) |
                      ftxui::color(focused ? palette::accent : palette::text),
                  text(" ")};
  if (extra) {
    row.push_back(std::move(extra));
    row.push_back(text(" "));
  }
  row.push_back(text(value) | ftxui::color(focused ? palette::text : palette::dim));
  Element e = hbox(row);
  return focused ? e | bgcolor(palette::raised) : e;
}

std::string noteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (note < 0 || note > 127) return "?";
  return std::string(names[note % 12]) + std::to_string(note / 12 - 2);
}

std::string fit(const std::string& s, int width) {
  // Count UTF-8 code points (each ion string is single-width per code point).
  std::vector<std::size_t> starts;
  for (std::size_t i = 0; i < s.size(); ++i)
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
  if (static_cast<int>(starts.size()) <= width) return s;
  if (width <= 0) return {};
  return s.substr(0, starts[static_cast<std::size_t>(width - 1)]) + "…";
}

}  // namespace ion::ui
