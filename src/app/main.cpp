// ion — terminal sound module for the OXI One MK2.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/pipewire_out.h"
#include "core/version.h"
#include "engine/module.h"
#include "library/fm_library.h"
#include "library/syx.h"
#include "midi/alsa_seq_in.h"
#include "project/config.h"
#include "project/paths.h"
#include "project/session.h"
#include "ui/app.h"

namespace {

struct Args {
  bool version = false;
  bool help = false;
  bool headless = false;
  bool listMidi = false;
  std::string project;
  std::string syxPath;
  std::string importPath;
  int voice = 1;          // 1-based
  std::string midiMatch;  // overrides the settings for this run
  bool midiAll = false;
  unsigned latency = 0;   // 0 = from settings
  int snapshotWidth = 0;  // --ui-snapshot WxH (debug)
  int snapshotHeight = 0;
  std::string snapshotScreen = "tracks";
  int snapshotTrack = 1;
};

void printUsage() {
  std::puts(
      "usage: ion [options]\n"
      "\n"
      "  --project NAME    open (or create) ~/ion/projects/NAME\n"
      "  --syx FILE        add a DX7 .syx bank to the library; on an empty project, play it on track 1\n"
      "  --voice N         with --syx: which voice of the bank goes on track 1 (default 1)\n"
      "  --import PATH     add every .syx/.snd under PATH to the FM library, print a summary, exit\n"
      "  --midi NAME       connect MIDI inputs whose name contains NAME (default: from Settings)\n"
      "  --midi-all        connect every hardware MIDI input\n"
      "  --latency FRAMES  requested audio buffer at 48 kHz (default: from Settings)\n"
      "  --headless        run without the UI; Ctrl+C to stop\n"
      "  --list-midi       list MIDI inputs and exit\n"
      "  --ui-snapshot WxH print one UI frame as text and exit (for bug reports)\n"
      "  --screen NAME     with --ui-snapshot: tracks, instrument, mod, mixer, fx, rec, library, settings, help, piano\n"
      "  --track N         with --ui-snapshot: the selected track, 1-8 (default 1)\n"
      "  --version         print the version and exit");
}

bool parseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "ion: %s needs a value\n", what);
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--version") a.version = true;
    else if (arg == "--help" || arg == "-h") a.help = true;
    else if (arg == "--headless") a.headless = true;
    else if (arg == "--list-midi") a.listMidi = true;
    else if (arg == "--midi-all") a.midiAll = true;
    else if (arg == "--project") { const char* v = value("--project"); if (!v) return false; a.project = v; }
    else if (arg == "--syx") { const char* v = value("--syx"); if (!v) return false; a.syxPath = v; }
    else if (arg == "--import") { const char* v = value("--import"); if (!v) return false; a.importPath = v; }
    else if (arg == "--voice") { const char* v = value("--voice"); if (!v) return false; a.voice = std::atoi(v); }
    else if (arg == "--midi") { const char* v = value("--midi"); if (!v) return false; a.midiMatch = v; }
    else if (arg == "--screen") { const char* v = value("--screen"); if (!v) return false; a.snapshotScreen = v; }
    else if (arg == "--track") { const char* v = value("--track"); if (!v) return false; a.snapshotTrack = std::atoi(v); }
    else if (arg == "--latency") {
      const char* v = value("--latency");
      if (!v) return false;
      a.latency = static_cast<unsigned>(std::atoi(v));
    } else if (arg == "--ui-snapshot") {
      const char* v = value("--ui-snapshot");
      if (!v || std::sscanf(v, "%dx%d", &a.snapshotWidth, &a.snapshotHeight) != 2) return false;
    } else {
      std::fprintf(stderr, "ion: unknown option %s (try --help)\n", arg.c_str());
      return false;
    }
  }
  return true;
}

bool loadBank(const std::string& path, std::vector<ion::syx::Voice>& voices) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "ion: can't open %s\n", path.c_str());
    return false;
  }
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  const auto parsed = ion::syx::parse(bytes);
  for (const auto& w : parsed.warnings) std::fprintf(stderr, "ion: %s: %s\n", path.c_str(), w.c_str());
  for (const auto& d : parsed.dumps) {
    const auto v = ion::syx::voices(d);
    voices.insert(voices.end(), v.begin(), v.end());
  }
  if (voices.empty()) {
    std::fprintf(stderr, "ion: no DX7 voices in %s\n", path.c_str());
    return false;
  }
  return true;
}

std::string baseName(const std::string& path) {
  const auto slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const auto dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

ion::ui::ScreenId screenFromName(const std::string& n) {
  if (n == "mixer") return ion::ui::ScreenId::Mixer;
  if (n == "instrument") return ion::ui::ScreenId::Instrument;
  if (n == "library") return ion::ui::ScreenId::Library;
  if (n == "settings") return ion::ui::ScreenId::Settings;
  if (n == "fx") return ion::ui::ScreenId::Fx;
  if (n == "rec") return ion::ui::ScreenId::Rec;
  if (n == "mod") return ion::ui::ScreenId::Mod;
  return ion::ui::ScreenId::Tracks;
}

volatile std::sig_atomic_t g_stop = 0;

}  // namespace

int main(int argc, char** argv) {
#if defined(__x86_64__) && defined(__AVX2__)
  // Built for AVX2/FMA CPUs (D-62): say so plainly on an older one instead of crashing.
  __builtin_cpu_init();
  if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma")) {
    std::fprintf(stderr, "ion: this build needs a CPU with AVX2 and FMA (Intel 2013 / AMD 2015 or newer).\n"
                         "Build it with -DION_PORTABLE=ON for older machines.\n");
    return 1;
  }
#endif
  Args args;
  if (!parseArgs(argc, argv, args)) return 2;
  if (args.help) {
    printUsage();
    return 0;
  }
  if (args.version) {
    std::puts(ion::versionLine().c_str());
    return 0;
  }
  if (args.listMidi) {
    std::string error;
    const auto ports = ion::AlsaSeqInput::listInputs(error);
    if (!error.empty()) {
      std::fprintf(stderr, "ion: %s\n", error.c_str());
      return 1;
    }
    for (const auto& p : ports) std::printf("%3d:%-2d  %s\n", p.client, p.port, p.name.c_str());
    return 0;
  }

  // ---- settings ----
  const ion::Paths paths = ion::Paths::defaults();
  std::vector<std::string> warnings;
  ion::Config config = ion::Config::load(paths.configFile, warnings);
  for (const auto& w : warnings) std::fprintf(stderr, "ion: %s\n", w.c_str());
  if (args.latency) config.latencyFrames = args.latency;

  if (!args.importPath.empty()) {
    const ion::ImportReport r = ion::FmLibrary::import(paths.libraryFile(), args.importPath, paths.importLog());
    if (!r.error.empty()) {
      std::fprintf(stderr, "ion: import: %s\n", r.error.c_str());
      return 1;
    }
    std::printf("%d files · %d banks · %d voices · %d new to the library\n", r.files, r.banks, r.voices, r.added);
    if (r.corrupt || r.unreadable)
      std::printf("%d corrupt dumps skipped, %d files without voice data: see %s\n", r.corrupt, r.unreadable,
                  paths.importLog().c_str());
    return 0;
  }

  std::vector<ion::syx::Voice> bank;
  if (!args.syxPath.empty()) {
    if (!loadBank(args.syxPath, bank)) return 1;
    const ion::ImportReport r = ion::FmLibrary::import(paths.libraryFile(), args.syxPath, paths.importLog());
    if (!r.error.empty()) std::fprintf(stderr, "ion: library: %s\n", r.error.c_str());
  }

  auto module = std::make_unique<ion::Module>();
  ion::Session session(*module, paths, config);
  std::string libraryError;
  if (!session.openLibrary(libraryError)) std::fprintf(stderr, "ion: FM library: %s (browsing disabled)\n", libraryError.c_str());

  // ---- project: --project, else the last one, else a new sketch ----
  std::string name = args.project;
  if (name.empty() && config.openLastProject) name = session.lastProject();
  if (!name.empty() && !ion::validProjectName(name)) {
    std::fprintf(stderr, "ion: \"%s\" isn't a valid project name\n", name.c_str());
    return 2;
  }
  std::string notice;
  std::string error;
  if (!name.empty() && std::filesystem::exists(paths.projectFile(name))) {
    if (!session.load(name, error)) {
      std::fprintf(stderr, "ion: can't open project \"%s\": %s\n", name.c_str(), error.c_str());
      return 1;
    }
    notice = "opened " + name;
  } else {
    session.newProject(name.empty() ? session.freshName() : name);
    notice = "new project: " + session.name() + " (w saves it)";
  }
  bool offerRecovery = false;
  if (session.hasRecovery(session.name())) {
    if (args.headless)
      std::fprintf(stderr, "ion: \"%s\" has unsaved changes in an autosave; run ion with the UI to recover them\n",
                   session.name().c_str());
    else
      offerRecovery = true;
  }

  // Quick start: a bank given to an empty project puts a voice on track 1.
  bool emptyProject = true;
  for (int t = 0; t < ion::kTrackCount; ++t) emptyProject &= session.track(t).engine == ion::EngineType::None;
  if (!bank.empty() && emptyProject) {
    session.setEngine(0, ion::EngineType::Fm);
    const int v = std::clamp(args.voice - 1, 0, static_cast<int>(bank.size()) - 1);
    session.setVoice(0, bank[static_cast<std::size_t>(v)], baseName(args.syxPath));
  }

  // ---- snapshot (debug aid, no audio) ----
  if (args.snapshotWidth > 0) {
    std::vector<float> buf(2 * 256);
    module->process(buf.data(), 256);  // apply the project
    session.waitForSamples(5000);
    session.tick();
    const bool piano = args.snapshotScreen == "piano", help = args.snapshotScreen == "help";
    std::fputs(ion::ui::snapshot(session, *module, screenFromName(args.snapshotScreen), args.snapshotWidth,
                                 args.snapshotHeight, piano, help, std::clamp(args.snapshotTrack, 1, ion::kTrackCount) - 1)
                   .c_str(),
               stdout);
    return 0;
  }

  // ---- audio + MIDI ----
  ion::PipeWireOutput audio(*module, {config.latencyFrames});
  const bool audioOk = audio.start(error);
  if (!audioOk) {
    std::fprintf(stderr, "ion: audio: %s\n", error.c_str());
    if (args.headless) return 1;
    notice = "no audio: " + error;
  }
  std::string match = args.midiMatch.empty() ? config.midiInput : args.midiMatch;
  if (args.midiAll || (args.midiMatch.empty() && config.midiAcceptAll)) match.clear();
  ion::AlsaSeqInput midi(*module, {match});
  const bool midiOk = midi.start(error);
  if (!midiOk) std::fprintf(stderr, "ion: midi: %s (continuing without MIDI)\n", error.c_str());

  if (args.headless) {
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::printf("%s · headless · project \"%s\" · Ctrl+C to stop\n", ion::versionLine().c_str(), session.name().c_str());
    const auto& st = module->status();
    int ticks = 0;
    while (!g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      session.tick();
      if (std::string m = session.takeMessage(); !m.empty()) std::printf("\n%s\n", m.c_str());
      if (++ticks % 10) continue;
      const auto names = midiOk ? midi.connected() : std::vector<std::string>{};
      char clock[32] = "no clock";
      if (st.clockPresent.load())
        std::snprintf(clock, sizeof clock, "%.1f BPM%s", static_cast<double>(st.bpm.load()), st.running.load() ? " ▶" : "");
      std::printf("\rcpu %4.1f%% (peak %4.1f%%) · %u frames · %u dropouts · %s · %s · midi %u · %s      ",
                  static_cast<double>(st.dspLoad.load() * 100), static_cast<double>(st.dspLoadPeak.load() * 100),
                  st.callbackFrames.load(), audio.dropouts(), audio.realtime() ? "RT" : "no RT", clock,
                  st.midiReceived.load(), names.empty() ? "no OXI" : names.front().c_str());
      std::fflush(stdout);
    }
    std::puts("");
  } else {
    ion::ui::run(session, *module, {audioOk ? &audio : nullptr, midiOk ? &midi : nullptr, offerRecovery, notice});
  }

  // A clean exit keeps unsaved work as an autosave; the next start offers it back.
  session.autosaveIfNeeded();
  midi.stop();
  audio.stop();
  return 0;
}
