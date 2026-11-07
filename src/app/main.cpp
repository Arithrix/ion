// ion — terminal sound module for the OXI One MK2.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/pipewire_out.h"
#include "core/version.h"
#include "engine/fm/fm_engine.h"
#include "engine/module.h"
#include "library/syx.h"
#include "midi/alsa_seq_in.h"
#include "ui/shell.h"

namespace {

struct Args {
  bool version = false;
  bool help = false;
  bool headless = false;
  bool listMidi = false;
  int snapshotWidth = 0;  // --ui-snapshot WxH (debug)
  int snapshotHeight = 0;
  std::string syxPath;
  int voice = 1;  // 1-based
  std::string midiMatch = "OXI";
  unsigned latency = 128;
};

void printUsage() {
  std::puts(
      "usage: ion [options]\n"
      "\n"
      "  --syx FILE        load a DX7 .syx bank onto track 1\n"
      "  --voice N         start on voice N of the bank (1-32)\n"
      "  --midi NAME       connect MIDI inputs whose name contains NAME (default OXI)\n"
      "  --midi-all        connect every hardware MIDI input\n"
      "  --latency FRAMES  requested audio buffer at 48 kHz (default 128)\n"
      "  --headless        run without the UI; Ctrl+C to stop\n"
      "  --list-midi       list MIDI inputs and exit\n"
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
    else if (arg == "--midi-all") a.midiMatch.clear();
    else if (arg == "--syx") { const char* v = value("--syx"); if (!v) return false; a.syxPath = v; }
    else if (arg == "--voice") { const char* v = value("--voice"); if (!v) return false; a.voice = std::atoi(v); }
    else if (arg == "--midi") { const char* v = value("--midi"); if (!v) return false; a.midiMatch = v; }
    else if (arg == "--ui-snapshot") {
      const char* v = value("--ui-snapshot");
      if (!v || std::sscanf(v, "%dx%d", &a.snapshotWidth, &a.snapshotHeight) != 2) return false;
    } else if (arg == "--latency") {
      const char* v = value("--latency");
      if (!v) return false;
      a.latency = static_cast<unsigned>(std::atoi(v));
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

volatile std::sig_atomic_t g_stop = 0;

}  // namespace

int main(int argc, char** argv) {
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

  std::vector<ion::syx::Voice> voices;
  if (!args.syxPath.empty() && !loadBank(args.syxPath, voices)) return 1;
  int voiceIndex = 0;
  if (!voices.empty()) voiceIndex = std::clamp(args.voice - 1, 0, static_cast<int>(voices.size()) - 1);

  ion::Module module;
  auto fm = std::make_unique<ion::FmEngine>(8);
  if (!voices.empty()) fm->loadVoice(voices[static_cast<std::size_t>(voiceIndex)]);
  module.setFmEngine(0, std::move(fm));

  ion::PipeWireOutput audio(module, {args.latency});
  std::string error;
  if (!audio.start(error)) {
    std::fprintf(stderr, "ion: audio: %s\n", error.c_str());
    return 1;
  }

  ion::AlsaSeqInput midi(module, {args.midiMatch});
  const bool midiOk = midi.start(error);
  if (!midiOk) std::fprintf(stderr, "ion: midi: %s (continuing without MIDI)\n", error.c_str());

  if (args.snapshotWidth > 0) {
    // Debug aid: play a note, then print one UI frame as text.
    ion::Command c;
    c.type = ion::Command::Type::NoteOn;
    c.note = 60;
    c.velocity = 100;
    module.pushCommand(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ion::ShellContext ctx{module, &audio, midiOk ? &midi : nullptr, voices, baseName(args.syxPath), voiceIndex};
    std::fputs(ion::snapshot(ctx, args.snapshotWidth, args.snapshotHeight).c_str(), stdout);
    c.type = ion::Command::Type::NoteOff;
    module.pushCommand(c);
  } else if (args.headless) {
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::printf("%s · headless · Ctrl+C to stop\n", ion::versionLine().c_str());
    if (!voices.empty())
      std::printf("track 1: FM · %s\n", ion::syx::name(voices[static_cast<std::size_t>(voiceIndex)]).c_str());
    const auto& st = module.status();
    while (!g_stop) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const auto names = midiOk ? midi.connected() : std::vector<std::string>{};
      std::printf("\rcpu %4.1f%% (peak %4.1f%%) · %u frames · %u dropouts · %s · midi %u msgs · %s      ",
                  st.dspLoad.load() * 100, st.dspLoadPeak.load() * 100, st.callbackFrames.load(),
                  audio.dropouts(), audio.realtime() ? "RT" : "no RT", st.midiReceived.load(),
                  names.empty() ? "no OXI" : names.front().c_str());
      std::fflush(stdout);
    }
    std::puts("");
  } else {
    ion::ShellContext ctx{module, &audio, midiOk ? &midi : nullptr, voices, baseName(args.syxPath), voiceIndex};
    ion::runShell(ctx);
  }

  midi.stop();
  audio.stop();
  return 0;
}
