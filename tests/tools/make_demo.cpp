// make-demo: builds the demo project for a release from a DX7 bank on this
// machine (the factory patches can't live in the repo). Used by scripts/release.sh.
//
//   make-demo --syx ~/ion/downloads/dx7/x/sysex/rom1a.syx --projects ~/ion/projects --name "demo 0.1.0"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "engine/fm/fm_engine.h"
#include "engine/module.h"
#include "library/syx.h"
#include "project/config.h"
#include "project/paths.h"
#include "project/session.h"

using namespace ion;

int main(int argc, char** argv) {
  std::string syxPath, projects, name;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--syx") syxPath = argv[i + 1];
    else if (a == "--projects") projects = argv[i + 1];
    else if (a == "--name") name = argv[i + 1];
  }
  if (syxPath.empty() || projects.empty() || name.empty()) {
    std::fputs("usage: make-demo --syx ROM1A.syx --projects DIR --name NAME\n", stderr);
    return 2;
  }
  std::ifstream f(syxPath, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  const auto parsed = syx::parse(bytes);
  if (parsed.dumps.empty()) {
    std::fprintf(stderr, "make-demo: no DX7 bank in %s\n", syxPath.c_str());
    return 1;
  }
  const auto bank = syx::voices(parsed.dumps.front());
  if (bank.size() < 32) {
    std::fprintf(stderr, "make-demo: %s isn't a 32-voice bank\n", syxPath.c_str());
    return 1;
  }

  auto module = std::make_unique<Module>();
  Config config;
  config.autosaveMinutes = 0;
  Paths paths = Paths::defaults();
  paths.projectsDir = projects;
  Session s(*module, paths, config);
  s.newProject(name);

  auto fm = [&](int track, int voice, int channel) {
    s.setEngine(track, EngineType::Fm);
    s.setVoice(track, bank[static_cast<std::size_t>(voice)], "rom1a");
    s.setChannel(track, channel);
  };
  auto set = [&](int track, MixerParam p, float v) { s.setParam({static_cast<uint8_t>(track), p}, v); };

  // ROM1A order: 1 BRASS 1 … 4 STRINGS 1 … 11 E.PIANO 1 … 15 BASS 1.
  fm(0, 10, 0);  // E.PIANO 1 on ch 1
  set(0, kFader, -4.0f);
  set(0, kPan, -0.15f);
  set(0, kHpOn, 1.0f);
  set(0, kHpFreq, 80.0f);
  set(0, kB1Freq, 2500.0f);
  set(0, kB1Gain, 2.5f);
  set(0, kB1Q, 0.8f);

  fm(1, 14, 1);  // BASS 1 on ch 2
  set(1, kFader, -2.0f);
  set(1, kLsFreq, 90.0f);
  set(1, kLsGain, 3.0f);
  set(1, kLpOn, 1.0f);
  set(1, kLpFreq, 6000.0f);

  fm(2, 0, 2);  // BRASS 1 on ch 3
  set(2, kFader, -8.0f);
  set(2, kPan, 0.25f);

  fm(3, 3, 0);  // STRINGS 1 layered under the piano, also ch 1
  set(3, kFader, -14.0f);
  set(3, kHpOn, 1.0f);
  set(3, kHpFreq, 300.0f);

  // Two mappings to try from the OXI: cc 74 on channel 1 sweeps the piano's
  // presence bell, cc 7 on channel 2 rides the bass fader (kept audible).
  s.addMapping(0, 74, {0, kB1Gain});
  s.addMapping(1, 7, {1, kFader}, 0.4f, 1.0f);

  std::string error;
  if (!s.save(error)) {
    std::fprintf(stderr, "make-demo: %s\n", error.c_str());
    return 1;
  }
  std::printf("make-demo: wrote %s (%zu cc mappings)\n", paths.projectFile(name).c_str(), s.ccMappings().size());
  return 0;
}
