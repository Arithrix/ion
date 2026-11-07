// ion-render: renders a project and a MIDI file to a WAV, with no audio device
// (HANDOFF §11). Same engine, same MIDI dispatch as live playing.
//
//   ion-render --project NAME|FILE.json --midi FILE.mid --out FILE.wav [--tail SECONDS]

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "core/version.h"
#include "engine/module.h"
#include "project/config.h"
#include "project/offline.h"
#include "project/paths.h"
#include "project/session.h"

int main(int argc, char** argv) {
  std::string project, midiPath, outPath;
  double tail = 2.0;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--project") project = argv[i + 1];
    else if (a == "--midi") midiPath = argv[i + 1];
    else if (a == "--out") outPath = argv[i + 1];
    else if (a == "--tail") tail = std::atof(argv[i + 1]);
  }
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::puts(ion::versionLine().c_str());
    return 0;
  }
  if (project.empty() || midiPath.empty() || outPath.empty()) {
    std::fputs("usage: ion-render --project NAME|FILE.json --midi FILE.mid --out FILE.wav [--tail SECONDS]\n", stderr);
    return 2;
  }

  auto module = std::make_unique<ion::Module>();
  ion::Config config;
  ion::Session session(*module, ion::Paths::defaults(), config);
  std::string error;
  const bool isFile = project.size() > 5 && project.substr(project.size() - 5) == ".json";
  if (isFile) {
    std::string text;
    if (!ion::readFile(project, text) || !session.fromJson(text, error)) {
      std::fprintf(stderr, "ion-render: %s: %s\n", project.c_str(), error.empty() ? "can't read" : error.c_str());
      return 1;
    }
  } else if (!session.load(project, error)) {
    std::fprintf(stderr, "ion-render: %s\n", error.c_str());
    return 1;
  }

  // Drum samples load on the disk thread; render only once they're all in.
  if (!session.waitForSamples(30000)) {
    std::fprintf(stderr, "ion-render: samples still loading after 30 s\n");
    return 1;
  }
  for (int t = 0; t < ion::kTrackCount; ++t)
    for (int pad = 0; pad < ion::DrumEngine::kPads; ++pad)
      if (session.padStatus(t, pad) == ion::Session::PadStatus::Failed)
        std::fprintf(stderr, "ion-render: track %d pad %d: %s (%s)\n", t + 1, pad + 1, session.padError(t, pad).c_str(),
                     session.track(t).pads[static_cast<std::size_t>(pad)].sample.c_str());

  std::ifstream f(midiPath, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  ion::smf::File midi;
  if (!ion::smf::parse(bytes, midi, error)) {
    std::fprintf(stderr, "ion-render: %s: %s\n", midiPath.c_str(), error.c_str());
    return 1;
  }

  const double seconds = midi.lengthSeconds + tail;
  const ion::wav::Audio audio = ion::renderOffline(*module, session, midi.events, seconds);
  if (!ion::wav::write(outPath, audio, error)) {
    std::fprintf(stderr, "ion-render: %s\n", error.c_str());
    return 1;
  }
  std::printf("%s: %.2f s, %zu MIDI events\n", outPath.c_str(), seconds, midi.events.size());
  return 0;
}
