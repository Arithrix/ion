#include "ui/shell.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "audio/pipewire_out.h"
#include "core/version.h"
#include "engine/module.h"
#include "midi/alsa_seq_in.h"

namespace ion {
namespace {

using namespace ftxui;

// Colour tokens (HANDOFF §8).
const Color kBg = Color::RGB(0x0F, 0x12, 0x18);
const Color kRaised = Color::RGB(0x1B, 0x22, 0x2C);
const Color kBorder = Color::RGB(0x39, 0x42, 0x4F);
const Color kText = Color::RGB(0xD8, 0xDE, 0xE9);
const Color kDim = Color::RGB(0x8B, 0x95, 0xA7);
const Color kFaint = Color::RGB(0x5C, 0x66, 0x77);
const Color kAccent = Color::RGB(0xF0, 0xA0, 0x4B);
const Color kValue = Color::RGB(0x6C, 0xC5, 0xD9);
const Color kRecord = Color::RGB(0xFF, 0x7A, 0x7A);

constexpr uint8_t kAuditionNote = 60;  // C3 in the Yamaha naming the OXI uses
constexpr auto kAuditionLength = std::chrono::milliseconds(300);

std::string noteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(names[note % 12]) + std::to_string(note / 12 - 2);
}

std::string describeMidi(uint32_t packed) {
  const int status = packed & 0xFF, d1 = (packed >> 8) & 0xFF, d2 = (packed >> 16) & 0xFF;
  if (status == 0) return "nothing yet";
  char buf[64];
  const int ch = (status & 0x0F) + 1;
  switch (status & 0xF0) {
    case 0x90: std::snprintf(buf, sizeof buf, "note on ch%02d %s vel %d", ch, noteName(d1).c_str(), d2); break;
    case 0x80: std::snprintf(buf, sizeof buf, "note off ch%02d %s", ch, noteName(d1).c_str()); break;
    case 0xB0: std::snprintf(buf, sizeof buf, "cc ch%02d #%d = %d", ch, d1, d2); break;
    case 0xC0: std::snprintf(buf, sizeof buf, "program ch%02d %d", ch, d1 + 1); break;
    case 0xE0: std::snprintf(buf, sizeof buf, "pitch bend ch%02d %d", ch, ((d2 << 7) | d1) - 8192); break;
    default:
      if (status == 0xF8) return "clock";
      if (status == 0xFA) return "start";
      if (status == 0xFB) return "continue";
      if (status == 0xFC) return "stop";
      std::snprintf(buf, sizeof buf, "0x%02X", status);
  }
  return buf;
}

float toDb(float linear) { return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f; }

// A level bar `width` cells wide over −48…0 dB, with eighth-cell resolution.
Element meter(float linear, int width, Color fill) {
  static const char* partial[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
  const float db = toDb(linear);
  const float frac = std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f);
  const int eighths = static_cast<int>(std::lround(frac * static_cast<float>(width * 8)));
  std::string on;
  for (int i = 0; i < eighths / 8; ++i) on += "█";
  const int rest = eighths % 8;
  if (rest) on += partial[rest];
  const int used = eighths / 8 + (rest ? 1 : 0);
  std::string off;
  for (int i = used; i < width; ++i) off += "░";
  return hbox({text(on) | color(db > -3.0f ? kAccent : fill), text(off) | color(kBorder)});
}

Element pane(const std::string& title, Element content, bool focused) {
  const Color c = focused ? kAccent : kBorder;
  return window(text(" " + title + " ") | bold | color(focused ? kAccent : kDim), content | color(kText)) |
         color(c);
}

Element hintBar(const std::vector<std::pair<std::string, std::string>>& hints) {
  Elements items;
  for (const auto& [key, label] : hints) {
    items.push_back(text(" " + key + " ") | bold | color(kText));
    items.push_back(text(" " + label + " ") | color(kBg) | bgcolor(kValue));
    items.push_back(text(" "));
  }
  return hbox(items);
}

// UI state that survives between frames. Only touched on the UI thread.
struct ViewState {
  int selected = 0;
  std::string flash;  // one-line message in the status line
  std::array<uint32_t, kTrackCount> lastNotes{};
  std::array<std::chrono::steady_clock::time_point, kTrackCount> lastActivity{};
};

// The Tracks screen: one frame, from the module's current status.
Element view(ShellContext& ctx, ViewState& vs, int width) {
  const bool wide = width >= 110;   // two panes and the full header
  const bool narrow = width < 80;   // the Comet: one pane, short labels
  Module& module = ctx.module;
  const ModuleStatus& st = module.status();
  // ---- header ----
  const float load = st.dspLoad.load(std::memory_order_relaxed);
  char loadText[32];
  std::snprintf(loadText, sizeof loadText, "%d%%", static_cast<int>(std::lround(load * 100)));
  std::string audioInfo = "no audio";
  if (ctx.audio) {
    audioInfo = (wide ? "48k · " : "") + std::to_string(st.callbackFrames.load()) + (wide ? " · pipewire" : " pw");
    if (!ctx.audio->realtime()) audioInfo += wide ? " · no RT" : " !RT";
  }
  Elements tabs;
  const char* longNames[][2] = {{"T", "racks"}, {"I", "nstr"}, {"M", "ixer"}, {"F", "x"}, {"R", "ec"}, {"L", "ibrary"}};
  const char* shortNames[][2] = {{"T", "rk"}, {"I", "ns"}, {"M", "ix"}, {"F", "x"}, {"R", "ec"}, {"L", "ib"}};
  const auto& tabNames = narrow ? shortNames : longNames;
  for (int i = 0; i < 6; ++i) {
    const bool on = i == 0;
    tabs.push_back(hbox({text(tabNames[i][0]) | bold | color(on ? kBg : kAccent),
                         text(tabNames[i][1]) | color(on ? kBg : kDim)}) |
                   bgcolor(on ? kAccent : kBg));
    tabs.push_back(text(" "));
  }
  const uint32_t dropouts = ctx.audio ? ctx.audio->dropouts() : 0;
  auto header = hbox({
      text(" ion") | bold | color(kAccent),
      text(wide ? "  sound module   " : "  ") | color(kDim),
      hbox(tabs),
      filler(),
      text(narrow ? "" : "cpu ") | color(kDim),
      text(loadText) | color(kText),
      text(narrow ? " " : "   "),
      text(dropouts ? std::to_string(dropouts) + " dropouts   " : "") | color(kRecord),
      text(audioInfo + " ") | color(kDim),
  });

  // ---- tracks ----
  Elements rows;
  rows.push_back(hbox({text(" #  ch  engine  ") , text("sound") | flex, text(narrow ? "level     " : "level       "), text("    ")}) |
                 color(kDim));
  const auto now = std::chrono::steady_clock::now();
  for (int t = 0; t < kTrackCount; ++t) {
    const bool sel = t == vs.selected;
    const bool has = module.hasEngine(t);
    const uint32_t notes = st.trackNotes[t].load(std::memory_order_relaxed);
    if (notes != vs.lastNotes[t]) {
      vs.lastNotes[t] = notes;
      vs.lastActivity[t] = now;
    }
    const bool active = now - vs.lastActivity[t] < std::chrono::milliseconds(150);
    char head[32];
    std::snprintf(head, sizeof head, " %d  %02d  ", t + 1, module.channel(t) + 1);
    std::string sound = "— empty —";
    if (has && t == 0)
      sound = ctx.voices.empty() ? "INIT VOICE" : syx::name(ctx.voices[static_cast<std::size_t>(ctx.voiceIndex)]);
    auto row = hbox({
        text(head),
        text(has ? "FM      " : "—       ") | bold | color(has ? kValue : kFaint),
        text(sound) | color(has ? kText : kFaint) | flex,
        meter(st.trackPeak[t].load(std::memory_order_relaxed), narrow ? 6 : 10, kValue),
        text(active ? "  ● " : "  · ") | color(active ? kValue : kFaint),
    });
    rows.push_back(sel ? row | bgcolor(kRaised) | color(kAccent) : row);
  }
  rows.push_back(filler());
  char voicesText[64];
  std::snprintf(voicesText, sizeof voicesText, " %d voices playing", st.voices.load());
  rows.push_back(separatorLight() | color(kBorder));
  rows.push_back(text(voicesText) | color(kDim));
  auto tracks = pane("Tracks", vbox(rows), true) | flex;

  // ---- detail ----
  Elements detail;
  if (module.hasEngine(vs.selected) && vs.selected == 0) {
    const std::string name =
        ctx.voices.empty() ? "INIT VOICE" : syx::name(ctx.voices[static_cast<std::size_t>(ctx.voiceIndex)]);
    detail.push_back(text(name) | bold | color(kValue));
    if (!ctx.voices.empty()) {
      const auto& v = ctx.voices[static_cast<std::size_t>(ctx.voiceIndex)];
      detail.push_back(text("voice " + std::to_string(ctx.voiceIndex + 1) + " of " +
                            std::to_string(ctx.voices.size()) + " · from " + ctx.bankName) |
                       color(kDim));
      detail.push_back(text(" "));
      detail.push_back(hbox({text("algorithm  ") | color(kDim), text(std::to_string(v[134] + 1))}));
      detail.push_back(hbox({text("feedback   ") | color(kDim), text(std::to_string(v[135]))}));
      const int transpose = static_cast<int>(v[144]) - 24;  // 24 = no transpose
      detail.push_back(hbox({text("transpose  ") | color(kDim),
                             text((transpose > 0 ? "+" : "") + std::to_string(transpose) + " st")}));
    } else {
      detail.push_back(text("no bank loaded · start with --syx FILE") | color(kDim));
    }
    detail.push_back(filler());
    detail.push_back(separatorLight() | color(kBorder));
    detail.push_back(text("midi ch " + std::to_string(module.channel(vs.selected) + 1) + " · poly 8") | color(kDim));
  } else {
    detail.push_back(text("empty track") | color(kFaint));
    detail.push_back(text("engines arrive in later phases") | color(kFaint));
    detail.push_back(filler());
  }
  auto detailPane = pane("Track " + std::to_string(vs.selected + 1) + (module.hasEngine(vs.selected) ? " · FM" : ""),
                         vbox(detail), false) |
                    size(WIDTH, GREATER_THAN, 34);

  // ---- master ----
  const float pl = st.peakLeft.load(), pr = st.peakRight.load();
  char peakText[32];
  std::snprintf(peakText, sizeof peakText, " %6.1f dB", std::max(-99.9f, toDb(std::max(pl, pr))));
  const uint32_t clips = st.clippedBlocks.load();
  auto master = pane("Master",
                     hbox({text("OUT L ") | color(kDim), meter(pl, narrow ? 12 : 20, kValue), text("  R ") | color(kDim),
                           meter(pr, narrow ? 12 : 20, kValue), text(peakText) | color(kDim), filler(),
                           text(clips ? "limited " + std::to_string(clips) + "× " : "") | color(kRecord)}),
                     false);

  // ---- status + hints ----
  std::string midiText;
  if (ctx.midi) {
    const auto names = ctx.midi->connected();
    if (names.empty()) midiText = narrow ? "no OXI found" : "no OXI found — plug it in, it connects automatically";
    else {
      midiText = names.front();
      if (names.size() > 1) midiText += " +" + std::to_string(names.size() - 1);
    }
  } else {
    midiText = "MIDI off";
  }
  auto status = hbox({text(" midi ") | color(kDim), text(midiText) | color(kText),
                      text(narrow ? " · " : "  ·  last: ") | color(kDim),
                      text(describeMidi(st.lastMidi.load())) | color(kText), filler(),
                      text(vs.flash + " ") | color(kAccent)});
  auto hints = hintBar({{"↑↓", "Track"}, {"←→", "Patch"}, {"Spc", "Audition"}, {"p", "Panic"}, {"q", "Quit"}});

  return vbox({
             header,
             (narrow ? tracks : hbox({tracks, detailPane})) | flex,
             master,
             status,
             hints,
         }) |
         bgcolor(kBg) | color(kText);
}

}  // namespace

int runShell(ShellContext& ctx) {
  auto screen = ScreenInteractive::Fullscreen();
  Module& module = ctx.module;

  ViewState vs;
  int& selected = vs.selected;
  // Pending audition note-offs. Only touched on the UI thread.
  std::vector<std::pair<std::chrono::steady_clock::time_point, uint8_t>> pendingOffs;

  auto sendVoice = [&] {
    if (ctx.voices.empty()) return;
    Command c;
    c.type = Command::Type::LoadVoice;
    c.track = 0;
    c.voice = ctx.voices[static_cast<std::size_t>(ctx.voiceIndex)];
    module.pushCommand(c);
  };

  auto renderer = Renderer([&] { return view(ctx, vs, Terminal::Size().dimx); });

  auto app = CatchEvent(renderer, [&](Event e) {
    if (e == Event::Custom) {
      const auto now = std::chrono::steady_clock::now();
      std::erase_if(pendingOffs, [&](const auto& p) {
        if (p.first > now) return false;
        Command c;
        c.type = Command::Type::NoteOff;
        c.note = p.second;
        module.pushCommand(c);
        return true;
      });
      return true;
    }
    if (e == Event::Character('q') || e == Event::CtrlC) {
      screen.Exit();
      return true;
    }
    if (e == Event::ArrowUp) { selected = (selected + kTrackCount - 1) % kTrackCount; return true; }
    if (e == Event::ArrowDown) { selected = (selected + 1) % kTrackCount; return true; }
    if (e.is_character() && e.character().size() == 1 && e.character()[0] >= '1' && e.character()[0] <= '8') {
      selected = e.character()[0] - '1';
      return true;
    }
    const bool shift = e == Event::ArrowLeftCtrl || e == Event::ArrowRightCtrl;
    if ((e == Event::ArrowLeft || e == Event::ArrowRight || shift) && selected == 0 && !ctx.voices.empty()) {
      const int step = (e == Event::ArrowLeft || e == Event::ArrowLeftCtrl) ? -1 : 1;
      const int n = static_cast<int>(ctx.voices.size());
      ctx.voiceIndex = (ctx.voiceIndex + step * (shift ? 8 : 1) + n * 8) % n;
      sendVoice();
      return true;
    }
    if (e == Event::Character(' ') && module.hasEngine(selected)) {
      Command c;
      c.type = Command::Type::NoteOn;
      c.track = static_cast<uint8_t>(selected);
      c.note = kAuditionNote;
      c.velocity = 100;
      module.pushCommand(c);
      pendingOffs.emplace_back(std::chrono::steady_clock::now() + kAuditionLength, kAuditionNote);
      return true;
    }
    if (e == Event::Character('p')) {
      Command c;
      c.type = Command::Type::Panic;
      module.pushCommand(c);
      vs.flash = "panic: all notes off";
      return true;
    }
    return false;
  });

  // ~30 fps refresh, and a tick for the audition timers.
  std::atomic<bool> running{true};
  std::thread ticker([&] {
    while (running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
      screen.PostEvent(Event::Custom);
    }
  });

  screen.Loop(app);
  running = false;
  ticker.join();
  return 0;
}

std::string snapshot(ShellContext& context, int width, int height) {
  ViewState vs;
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, view(context, vs, width));
  std::string out;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::string& c = screen.CellAt(x, y).character;
      out += c.empty() ? " " : c;
    }
    out += '\n';
  }
  return out;
}

}  // namespace ion
