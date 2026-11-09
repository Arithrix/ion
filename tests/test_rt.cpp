// The audio thread must never allocate or free (HANDOFF §4). This replaces
// operator new and delete for the test binary and counts any made while
// Module::process runs, with every engine, effect and feature busy.
//
// AddressSanitizer replaces these operators itself, so under it the guard
// is off and the test only exercises the paths.

#include <doctest.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <random>
#include <vector>

#include "audio/wav.h"
#include "engine/acid/acid_engine.h"
#include "engine/chip/chip_engine.h"
#include "engine/fx/effects.h"
#include "engine/module.h"
#include "engine/plaits/plaits_engine.h"
#include "project/config.h"
#include "project/session.h"

#if defined(__SANITIZE_ADDRESS__)
#define ION_RT_GUARD 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ION_RT_GUARD 0
#endif
#endif
#ifndef ION_RT_GUARD
#define ION_RT_GUARD 1
#endif

namespace {
thread_local bool g_audio = false;
std::atomic<long> g_audioAllocs{0}, g_audioFrees{0};
}  // namespace

#if ION_RT_GUARD
void* operator new(std::size_t n) {
  if (g_audio) ++g_audioAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (g_audio) ++g_audioAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a) {
  if (g_audio) ++g_audioAllocs;
  const std::size_t al = static_cast<std::size_t>(a);
  if (void* p = std::aligned_alloc(al, (n + al - 1) / al * al)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void* p) noexcept {
  if (g_audio && p) ++g_audioFrees;
  std::free(p);
}
namespace {
void release(void* p) noexcept {
  if (g_audio && p) ++g_audioFrees;
  std::free(p);
}
}  // namespace
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { release(p); }
#endif

using namespace ion;
namespace fs = std::filesystem;

TEST_CASE("real time: the guard itself counts what it should") {
  if (!ION_RT_GUARD) return;
  g_audio = true;
  auto* volatile p = new int(1);
  delete p;
  g_audio = false;
  CHECK(g_audioAllocs.load() == 1);
  CHECK(g_audioFrees.load() == 1);
  g_audioAllocs = 0;
  g_audioFrees = 0;
}

TEST_CASE("real time: the audio thread never allocates or frees, with everything busy") {
  const fs::path root = fs::temp_directory_path() / ("ion-rt-" + std::to_string(std::random_device{}()));
  fs::create_directories(root);
  {
    auto module = std::make_unique<Module>();
    Config config;
    config.autosaveMinutes = 0;
    Session s(*module, Paths::under(root), config);
    s.newProject("rt");
    // A sample for the drum pads and the sampler.
    wav::Audio a;
    a.channels = 2;
    a.samples.assign(2 * 9600, 0.0f);
    for (std::size_t i = 0; i < a.samples.size(); ++i) a.samples[i] = 0.4f * std::sin(0.02f * static_cast<float>(i));
    std::string error;
    REQUIRE(wav::write(root / "tone.wav", a, error));

    const EngineType engines[] = {EngineType::Fm, EngineType::Drum, EngineType::Sampler, EngineType::Plaits,
                                  EngineType::Acid, EngineType::Chip, EngineType::Plaits, EngineType::Fm};
    for (int t = 0; t < kTrackCount; ++t) {
      s.setEngine(t, engines[t]);
      s.setChannel(t, t);
      s.setInsert(t, 0, static_cast<EffectType>(1 + t % (kEffectTypes - 1)));
      s.setParam({static_cast<uint8_t>(t), static_cast<uint16_t>(kSendA)}, -10);
      s.setParam({static_cast<uint8_t>(t), static_cast<uint16_t>(kSendB)}, -10);
      // Modulation on every track: an LFO on the fader, an envelope on pan.
      s.setModTarget(t, 0, ParamRef{static_cast<uint8_t>(t), static_cast<uint16_t>(kFader)});
      s.setParam(routeRef(t, 0, kRouteSource), kSrcLfo1);
      s.setParam(routeRef(t, 0, kRouteDepth), 0.2f);
      s.setModTarget(t, 1, ParamRef{static_cast<uint8_t>(t), static_cast<uint16_t>(kPan)});
      s.setParam(routeRef(t, 1, kRouteSource), kSrcEnv1);
      s.setParam(routeRef(t, 1, kRouteDepth), 0.5f);
      s.setParam(lfoRef(t, 0, kLfoSync), 1);
    }
    for (int pad = 0; pad < 16; ++pad) s.setPadSample(1, pad, (root / "tone.wav").string());
    s.setPadSample(2, 0, (root / "tone.wav").string());
    s.setPlaitsPatches(3, {syx::initVoice()});
    s.setInsert(kMasterScope, 1, EffectType::Drive);
    s.addMapping(0, 74, engineParamRef(4, AcidEngine::kCutoff));
    s.addMapping(0, 71, engineParamRef(5, ChipEngine::kWarp));  // rebuilds CHIP's table on the audio thread
    s.toggleProgram(syx::initVoice(), "rt");
    s.setProgramChange(true);
    REQUIRE(s.waitForSamples());

    std::vector<float> out(2 * 256);
    auto process = [&](int blocks) {
      for (int i = 0; i < blocks; ++i) {
        g_audio = true;
        module->process(out.data(), 256);
        g_audio = false;
      }
      s.tick();  // the control thread's side: hand-backs, notifications
    };
    process(8);  // engines handed over, defaults applied

    // A take of the master running through it all.
    s.recSetup().source = RecSource::Master;
    s.recSetup().synced = true;
    s.recSetup().bars = 2;
    REQUIRE(s.arm(error));

    std::mt19937 rng(61);
    for (int round = 0; round < 400; ++round) {
      for (int k = 0; k < 6; ++k) {
        const auto ch = static_cast<uint8_t>(rng() % 8);
        const auto note = static_cast<uint8_t>(30 + rng() % 60);
        s.sendMidi({static_cast<uint8_t>(0x90 | ch), note, static_cast<uint8_t>(40 + rng() % 88)});
        if (rng() % 2) s.sendMidi({static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(30 + rng() % 60), 0});
      }
      s.sendMidi({0xB0, 74, static_cast<uint8_t>(rng() % 128)});
      s.sendMidi({0xB0, 71, static_cast<uint8_t>(rng() % 128)});
      s.sendMidi({static_cast<uint8_t>(0xE0 | rng() % 8), 0, static_cast<uint8_t>(rng() % 128)});
      if (round % 50 == 0) s.sendMidi({0xC0, 0, 0});  // a program change on track 1
      if (round == 10) s.sendMidi({0xFA, 0, 0});      // the transport starts…
      for (int tick = 0; tick < 6; ++tick) s.sendMidi({0xF8, 0, 0});  // …with clock
      // Edits from the UI, every few rounds: values, a model, an effect's settings.
      if (round % 5 == 0) {
        s.setParam(engineParamRef(3, PlaitsEngine::kModel), static_cast<float>(rng() % 24));
        s.setParam(engineParamRef(5, ChipEngine::kShape), static_cast<float>(rng() % 9));
        s.setParam(insertParamRef(kMasterScope, 1, Drive::kShape), static_cast<float>(rng() % 3));
        s.setParam(lfoRef(0, 0, kLfoRate), static_cast<float>(1 + rng() % 20));
      }
      process(2);
    }
    process(40);
    CHECK(g_audioAllocs.load() == 0);
    CHECK(g_audioFrees.load() == 0);
    s.cancelTake();
    process(4);
  }
  fs::remove_all(root);
}
