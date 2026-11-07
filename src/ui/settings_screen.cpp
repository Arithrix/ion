// Settings (,): audio, MIDI and clock, keyboard, project, track ↔ channel, CC map.
// Changes apply at once and are written to config.toml (or the project) straight away.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>

#include "audio/pipewire_out.h"
#include "engine/module.h"
#include "midi/alsa_seq_in.h"
#include "project/paths.h"
#include "project/session.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;

struct Field {
  std::string label;
  std::string value;
  std::function<void(int, bool)> nudge;  // ← → (direction, coarse); null = read-only
  std::function<void()> enter;           // ⏎
  std::function<void()> remove;          // x
};

struct Section {
  std::string title;
  std::vector<Field> fields;
  std::string note;
};

void saveConfig(Context& ctx) {
  std::string error;
  if (!ctx.session.config().save(ctx.session.paths().configFile, error)) ctx.flash = "can't save settings: " + error;
}

void applyMidiMatch(Context& ctx) {
  const Config& c = ctx.session.config();
  if (ctx.midi) ctx.midi->setMatch(c.midiAcceptAll ? "" : c.midiInput);
}

int stepIn(const std::vector<int>& steps, int current, int dir) {
  auto it = std::find(steps.begin(), steps.end(), current);
  int i = it == steps.end() ? 0 : static_cast<int>(it - steps.begin());
  return steps[static_cast<std::size_t>(std::clamp(i + dir, 0, static_cast<int>(steps.size()) - 1))];
}

class SettingsScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const auto sections = build(ctx);
    section_ = std::clamp(section_, 0, static_cast<int>(sections.size()) - 1);
    auto renderSection = [&](int i) {
      const Section& s = sections[static_cast<std::size_t>(i)];
      const bool focused = i == section_;
      Elements rows;
      const int labelWidth = i == 5 ? 26 : 14;  // the CC map
      for (std::size_t f = 0; f < s.fields.size(); ++f) {
        const Field& fd = s.fields[f];
        const bool on = focused && static_cast<int>(f) == field_;
        const std::string value = fd.nudge && on ? "◀ " + fd.value + " ▶" : fd.value;
        rows.push_back(paramRow(fd.label, value, on, labelWidth));
      }
      if (s.fields.empty()) rows.push_back(text("none") | color(palette::faint));
      if (!s.note.empty()) rows.push_back(paragraph(s.note) | color(palette::faint));
      const std::string title = ctx.narrow() ? s.title + " · " + std::to_string(i + 1) + "/" + std::to_string(sections.size())
                                             : s.title;
      return pane(title, vbox(rows), focused);
    };
    if (ctx.narrow()) return renderSection(section_) | flex;
    Element left = vbox({renderSection(0), renderSection(1), renderSection(2)});
    Element right = vbox({renderSection(3), renderSection(4), renderSection(5) | flex});
    return vbox({text(" " + ctx.session.paths().configFile.string()) | color(palette::faint),
                 hbox({left | flex, right | flex}) | flex});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    auto sections = build(ctx);
    const int n = static_cast<int>(sections.size());
    if (e == Event::Tab) { section_ = (section_ + 1) % n; field_ = 0; return true; }
    if (e == Event::TabReverse) { section_ = (section_ + n - 1) % n; field_ = 0; return true; }
    Section& sec = sections[static_cast<std::size_t>(section_)];
    const int count = static_cast<int>(sec.fields.size());
    if (count == 0) return false;
    if (e == Event::ArrowUp) { field_ = (field_ + count - 1) % count; return true; }
    if (e == Event::ArrowDown) { field_ = (field_ + 1) % count; return true; }
    field_ = std::min(field_, count - 1);
    Field& f = sec.fields[static_cast<std::size_t>(field_)];
    if (const Nudge nd = nudgeOf(e); nd.direction && f.nudge) {
      f.nudge(nd.direction, nd.coarse);
      return true;
    }
    if (e == Event::Return && f.enter) { f.enter(); return true; }
    if (e == Event::Character('x') && f.remove) {
      f.remove();
      field_ = std::max(0, field_ - 1);
      return true;
    }
    return false;
  }

  Hints hints(Context&) override {
    return {{"Tab", "Section"}, {"↑↓", "Field"}, {"←→", "Change"}, {"⏎", "Edit"}, {"x", "Remove"}, {"Esc", "Back"}, {"?", "Help"}};
  }

 private:
  std::vector<Section> build(Context& ctx) {
    Session& s = ctx.session;
    Config& c = s.config();
    std::vector<Section> out;

    // ---- audio ----
    {
      Section a{"Audio", {}, ""};
      a.fields.push_back({"backend", "PipeWire (native)", nullptr, nullptr, nullptr});
      a.fields.push_back({"device", "default output · follows the system", nullptr, nullptr, nullptr});
      a.fields.push_back({"sample rate", "48000", nullptr, nullptr, nullptr});
      a.fields.push_back({"buffer", std::to_string(c.latencyFrames) + " frames",
                          [&ctx, &c](int d, bool) {
                            c.latencyFrames = static_cast<unsigned>(stepIn({32, 64, 128, 256, 512, 1024}, static_cast<int>(c.latencyFrames), d));
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      const ModuleStatus& st = ctx.module.status();
      std::string running = "no audio";
      if (ctx.audio) {
        running = std::to_string(st.callbackFrames.load()) + " frames · " + (ctx.audio->realtime() ? "real-time" : "NOT real-time") +
                  " · " + std::to_string(ctx.audio->dropouts()) + " dropouts";
      }
      a.fields.push_back({"running", running, nullptr, nullptr, nullptr});
      char note[96];
      std::snprintf(note, sizeof note, "%u frames at 48 kHz = %.1f ms · buffer changes apply on restart", c.latencyFrames,
                    c.latencyFrames * 1000.0 / 48000.0);
      a.note = note;
      out.push_back(std::move(a));
    }

    // ---- MIDI ----
    {
      Section m{"MIDI + clock", {}, ""};
      m.fields.push_back({"input", c.midiAcceptAll ? "(all inputs)" : "names containing \"" + c.midiInput + "\"", nullptr,
                          [&ctx, &c] {
                            ctx.promptTitle = "MIDI input name contains";
                            ctx.promptInitial = c.midiInput;
                            ctx.promptDone = [](Context& cx, const std::string& v) {
                              if (v.empty()) return;
                              cx.session.config().midiInput = v;
                              saveConfig(cx);
                              applyMidiMatch(cx);
                            };
                            ctx.request = Context::Request::Prompt;
                          },
                          nullptr});
      m.fields.push_back({"also accept", c.midiAcceptAll ? "all other inputs" : "only that", [&ctx, &c](int, bool) {
                            c.midiAcceptAll = !c.midiAcceptAll;
                            saveConfig(ctx);
                            applyMidiMatch(ctx);
                          },
                          nullptr, nullptr});
      m.fields.push_back({"timing", c.timing == MidiTiming::Tight ? "tight (+1 buffer, no jitter)" : "immediate (lowest latency)",
                          [&ctx, &s, &c](int, bool) {
                            s.setTiming(c.timing == MidiTiming::Tight ? MidiTiming::Immediate : MidiTiming::Tight);
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      std::string devices = "none";
      if (ctx.midi) {
        const auto names = ctx.midi->connected();
        if (!names.empty()) {
          devices = names.front();
          if (names.size() > 1) devices += " +" + std::to_string(names.size() - 1);
        }
      }
      m.fields.push_back({"connected", devices, nullptr, nullptr, nullptr});
      const ModuleStatus& st = ctx.module.status();
      char clock[64];
      if (st.clockPresent.load()) std::snprintf(clock, sizeof clock, "%.1f BPM · %s", st.bpm.load(), st.running.load() ? "running" : "stopped");
      else std::snprintf(clock, sizeof clock, "no clock");
      m.fields.push_back({"clock", clock, nullptr, nullptr, nullptr});
      m.fields.push_back({"program change",
                          c.programChange ? "on · " + std::to_string(s.programs().size()) + " in this project's list" : "off",
                          [&ctx, &s](int, bool) {
                            s.setProgramChange(!s.config().programChange);
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      m.fields.push_back({"panic", "p sends all notes off", nullptr, nullptr, nullptr});
      m.note = "program change N on a track's channel loads entry N of the program list (+ in the FM browser adds)";
      out.push_back(std::move(m));
    }

    // ---- keyboard + project ----
    {
      Section k{"Keyboard + project", {}, ""};
      k.fields.push_back({"piano gate", std::to_string(c.gateMs) + " ms", [&ctx, &c](int d, bool coarse) {
                            c.gateMs = std::clamp(c.gateMs + d * (coarse ? 100 : 10), 20, 5000);
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      k.fields.push_back({"piano rows", c.lowerRow + " / " + c.upperRow, nullptr, nullptr, nullptr});
      k.fields.push_back({"FM preview", c.previewOnMove ? "play C3 when browsing loads a patch" : "off", [&ctx, &c](int, bool) {
                            c.previewOnMove = !c.previewOnMove;
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      k.fields.push_back({"autosave", c.autosaveMinutes ? "every " + std::to_string(c.autosaveMinutes) + " min" : "off",
                          [&ctx, &c](int d, bool) {
                            c.autosaveMinutes = std::clamp(c.autosaveMinutes + d, 0, 30);
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      k.fields.push_back({"start-up", c.openLastProject ? "open the last project" : "new project", [&ctx, &c](int, bool) {
                            c.openLastProject = !c.openLastProject;
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      k.note = "piano rows: edit config.toml for non-US keyboards";
      out.push_back(std::move(k));
    }

    // ---- track ↔ channel ----
    {
      Section t{"Track ↔ channel", {}, "two tracks can share a channel to layer sounds"};
      for (int i = 0; i < kTrackCount; ++i) {
        const TrackState& tr = s.track(i);
        std::string engine = tr.engine == EngineType::None ? "empty" : engineLabel(tr.engine);
        t.fields.push_back({"track " + std::to_string(i + 1), "ch " + std::to_string(tr.channel + 1) + "   " + engine,
                            [&s, i](int d, bool) { s.setChannel(i, s.track(i).channel + d); }, nullptr, nullptr});
      }
      out.push_back(std::move(t));
    }

    // ---- CC map ----
    {
      Section m{"CC map", {}, "learn: focus any value and press l, then move a control on the OXI"};
      for (const Session::CcEntry& e : s.ccMappings()) {
        char head[32];
        std::snprintf(head, sizeof head, "cc %3d %s", e.mapping.cc,
                      e.mapping.channel == kAnyChannel ? "any " : ("ch " + std::to_string(e.mapping.channel + 1)).c_str());
        char range[32];
        std::snprintf(range, sizeof range, "%.0f–%.0f%%", e.mapping.lo * 100.0, e.mapping.hi * 100.0);
        const int slot = e.slot;
        const CcMapping mapping = e.mapping;
        m.fields.push_back({std::string(head) + " " + s.paramLabel(mapping.target), range,
                            [&s, slot, mapping](int d, bool coarse) {
                              // ← → narrows/widens the top of the range.
                              s.setMappingRange(slot, mapping.lo, mapping.hi + 0.05f * static_cast<float>(d) * (coarse ? 4.0f : 1.0f));
                            },
                            [&s, slot, mapping] {
                              // ⏎ inverts the direction.
                              s.setMappingRange(slot, mapping.hi, mapping.lo);
                            },
                            [&s, slot] { s.removeMapping(slot); }});
      }
      out.push_back(std::move(m));
    }

    // ---- samples (drawn above the CC map, so it goes before it) ----
    {
      Section m{"Samples", {}, "roots are where the sample browser (I on a drum track) starts"};
      for (std::size_t i = 0; i < c.sampleRoots.size(); ++i) {
        std::error_code ec;
        const bool found = std::filesystem::is_directory(expandHome(c.sampleRoots[i]), ec);
        m.fields.push_back({"root " + std::to_string(i + 1), c.sampleRoots[i] + (found ? "" : " · not found"), nullptr, nullptr,
                            [&ctx, &c, i] {
                              c.sampleRoots.erase(c.sampleRoots.begin() + static_cast<std::ptrdiff_t>(i));
                              saveConfig(ctx);
                            }});
      }
      m.fields.push_back({"add a root", "⏎ adds a folder", nullptr,
                          [&ctx] {
                            ctx.promptTitle = "Add a sample folder · Tab completes";
                            ctx.promptInitial = "~/";
                            ctx.promptPath = true;
                            ctx.promptDone = [](Context& cx, const std::string& typed) {
                              std::string p = typed;
                              while (p.size() > 1 && p.back() == '/') p.pop_back();
                              std::error_code ec;
                              if (p.empty() || !std::filesystem::is_directory(expandHome(p), ec)) {
                                cx.flash = "not a folder: " + p;
                                return;
                              }
                              const std::string stored = abbreviateHome(expandHome(p).string());
                              auto& roots = cx.session.config().sampleRoots;
                              if (std::find(roots.begin(), roots.end(), stored) == roots.end()) roots.push_back(stored);
                              saveConfig(cx);
                              cx.flash = "sample root added: " + stored;
                            };
                            ctx.request = Context::Request::Prompt;
                          },
                          nullptr});
      m.fields.push_back({"preview", c.samplePreview ? "play files as you move onto them" : "off", [&ctx, &c](int, bool) {
                            c.samplePreview = !c.samplePreview;
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      m.fields.push_back({"preview level", std::to_string(c.previewLevelDb) + " dB", [&ctx, &c](int d, bool coarse) {
                            c.previewLevelDb = std::clamp(c.previewLevelDb + d * (coarse ? 6 : 1), -40, 0);
                            saveConfig(ctx);
                          },
                          nullptr, nullptr});
      out.insert(out.end() - 1, std::move(m));
    }
    return out;
  }

  int section_ = 0;
  int field_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeSettingsScreen() { return std::make_unique<SettingsScreen>(); }

}  // namespace ion::ui
