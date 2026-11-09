// Instrument (I) for a SAMPLER track (mockups/Sampler; HANDOFF §6.2): the
// sample's waveform with its start and loop points (and a bar ruler for
// recordings that know their tempo), then the Play, Amp, Filter and Mod
// settings. `o` opens the sample browser; an empty sampler starts in it.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "engine/module.h"
#include "engine/sampler/sampler_engine.h"
#include "library/recording.h"
#include "project/paths.h"
#include "project/session.h"
#include "engine/envelope_params.h"
#include "ui/envelope_view.h"
#include "ui/sample_view.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;
using P = SamplerEngine::Param;

struct Section {
  const char* title;
  std::vector<int> rows;
};
const std::array<Section, 4> kSections = {{
    {"Play", {P::kMode, P::kLoop, P::kStart, P::kLoopStart, P::kLoopEnd, P::kCrossfade, P::kSnap, P::kRoot, P::kLowKey, P::kHighKey,
              P::kTune, P::kFine, P::kGlide, P::kPoly, P::kBendRange, P::kLevel}},
    {"Amp", {P::kAmpAttack, P::kAmpHold, P::kAmpDecay, P::kAmpSustain, P::kAmpRelease, P::kVelAmp}},
    {"Filter", {P::kFilter, P::kCutoff, P::kReso, P::kKeyTrack, P::kEnvAmount, P::kFAttack, P::kFHold, P::kFDecay, P::kFSustain,
                P::kFRelease, P::kVelFilter}},
    {"Mod", {P::kLfoShape, P::kLfoSync, P::kLfoRate, P::kLfoDivision, P::kLfoPitch, P::kLfoCutoff, P::kLfoPan, P::kLfoLevel}},
}};

bool isPoint(int p) { return p == P::kStart || p == P::kLoopStart || p == P::kLoopEnd; }

const char* levels[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

std::string lfoShape(int shape, int width) {
  std::string out;
  uint32_t seed = 5;
  float hold = 0;
  for (int x = 0; x < width; ++x) {
    const float ph = std::fmod(static_cast<float>(x) / 6.0f, 1.0f);
    float v;
    switch (shape) {
      case SamplerEngine::kSine: v = std::sin(2 * 3.14159265f * ph); break;
      case SamplerEngine::kTriangle: v = ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph; break;
      case SamplerEngine::kSquare: v = ph < 0.5f ? 1.0f : -1.0f; break;
      default:
        if (x % 6 == 0) hold = static_cast<float>((seed = seed * 1664525u + 1013904223u) >> 8) / 8388608.0f - 1.0f;
        v = hold;
        break;
    }
    out += levels[std::clamp(static_cast<int>(std::lround((v + 1) * 4)), 1, 8)];
  }
  return out;
}

class SamplerScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const Session& s = ctx.session;
    const int t = ctx.track;
    const int body = ctx.height - 3;
    if (browsing(ctx)) return sampleBrowserPane(ctx, ctx.width, body, true, target(ctx)) | flex;
    const int waveRows = body >= 30 ? 4 : body >= 24 ? 3 : 2;
    const int waveHeight = waveRows + 6;
    Element wave = wavePane(ctx, waveRows);
    Elements cols;
    if (ctx.narrow()) {
      cols.push_back(sectionPane(ctx, section_, ctx.width, body - waveHeight) | flex);
    } else if (ctx.width < 110) {
      // Two at a time: Play + Amp, or Filter + Mod (Tab moves on).
      const int w = ctx.width / 2, first = section_ < 2 ? 0 : 2;
      for (int i = first; i < first + 2; ++i) cols.push_back(sectionPane(ctx, i, w, body - waveHeight) | size(WIDTH, EQUAL, w));
    } else {
      const int w = ctx.width / 4;
      for (int i = 0; i < 4; ++i) cols.push_back(sectionPane(ctx, i, w, body - waveHeight) | size(WIDTH, EQUAL, w));
    }
    (void)s;
    (void)t;
    return vbox({wave | size(HEIGHT, EQUAL, waveHeight), hbox(cols) | flex});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    if (browsing(ctx)) {
      if (e == Event::Escape && ctx.samples->filter.empty() && !ctx.samples->typing && hasSample(ctx)) {
        browsing_ = false;
        return true;
      }
      if (ctx.samples->typing) {
        SampleBrowse& b = *ctx.samples;
        if (e == Event::Return) { b.typing = false; return true; }
        if (e == Event::Escape) { b.typing = false; b.setFilter({}); return true; }
        if (e == Event::Backspace) {
          std::string f = b.filter;
          if (!f.empty()) f.pop_back();
          b.setFilter(f);
          return true;
        }
        if (e.is_character()) { b.setFilter(b.filter + e.character()); return true; }
        if (e == Event::ArrowUp || e == Event::ArrowDown) b.typing = false;
      }
      if (e == Event::Character('o') && hasSample(ctx)) {
        browsing_ = false;
        return true;
      }
      return sampleBrowserEvent(ctx, e, target(ctx));
    }

    if (e == Event::Character('o')) {
      // Open where the current sample is, on it.
      const std::filesystem::path current = s.resolveSample(s.track(ctx.track).pads[0].sample);
      if (!current.empty()) ctx.samples->show(s, current.parent_path(), current);
      browsing_ = true;
      return true;
    }
    if (e == Event::Tab) { section_ = (section_ + 1) % 4; return true; }
    if (e == Event::TabReverse) { section_ = (section_ + 3) % 4; return true; }
    const auto& rows = kSections[static_cast<std::size_t>(section_)].rows;
    int& row = row_[static_cast<std::size_t>(section_)];
    const int n = static_cast<int>(rows.size());
    if (e == Event::ArrowUp) { row = (row + n - 1) % n; return true; }
    if (e == Event::ArrowDown) { row = (row + 1) % n; return true; }
    if (e == Event::Return) { ctx.request = Context::Request::Audition; return true; }
    if (e == Event::Character('z')) {
      zoom_ = (zoom_ + 1) % 4;
      return true;
    }
    if (e == Event::Character('g')) {
      std::string error;
      const int copied = s.collectSamples(error);
      if (copied < 0) ctx.flash = "not collected: " + error;
      else ctx.flash = copied == 0 ? "nothing to collect: every sample is already in the project"
                                   : "copied " + std::to_string(copied) + " sample" + (copied == 1 ? "" : "s") + " into the project's samples/ · w saves";
      return true;
    }
    if (e == Event::Character('n')) {
      const SampleLoader::Ptr smp = s.padSample(ctx.track, 0);
      if (smp && smp->peak > 0) {
        const float db = std::clamp(-20.0f * std::log10(smp->peak), -24.0f, 24.0f);
        s.setParam(engineParamRef(ctx.track, P::kLevel), db);
        char buf[48];
        std::snprintf(buf, sizeof buf, "normalized: level %+.1f dB (peak to 0 dBFS)", static_cast<double>(db));
        ctx.flash = buf;
      }
      return true;
    }
    if (const Nudge nd = nudgeOf(e); nd.direction) {
      const int p = rows[static_cast<std::size_t>(row)];
      if (isPoint(p)) movePoint(ctx, p, nd.direction, nd.coarse);
      else s.nudgeParam(engineParamRef(ctx.track, p), nd.direction, nd.coarse);
      return true;
    }
    return false;
  }

  Hints hints(Context& ctx) override {
    if (browsing(ctx)) {
      if (ctx.samples->typing) return {{"type", "Filter"}, {"⏎", "Done"}, {"Esc", "Clear"}};
      Hints h = {{"↑↓", "Browse"}, {"⏎", "Load"}, {"←", "Up"}, {"/", "Filter"}};
      if (hasSample(ctx)) h.push_back({"o", "Back to sample"});
      h.push_back({"Esc", "Back"});
      return h;
    }
    Hints h = {{"Tab", "Section"}, {"↑↓", "Param"}, {"←→", "Adjust"}, {"l", "Learn"}, {"z", "Zoom"}, {"n", "Normalize"}, {"o", "Open file"},
               {"g", "Collect"}, {"Spc", "Audition"}, {"Esc", "Back"}};
    return h;
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    if (browsing(ctx)) return std::nullopt;
    const auto& rows = kSections[static_cast<std::size_t>(section_)].rows;
    return engineParamRef(ctx.track, rows[static_cast<std::size_t>(row_[static_cast<std::size_t>(section_)])]);
  }

  bool capturesText(Context& ctx) override { return browsing(ctx) && ctx.samples->typing; }

 private:
  bool hasSample(Context& ctx) const { return !ctx.session.track(ctx.track).pads[0].sample.empty(); }
  bool browsing(Context& ctx) const { return browsing_ || !hasSample(ctx); }

  BrowserTarget target(Context& ctx) {
    BrowserTarget bt;
    const int t = ctx.track;
    bt.focus = [](Context&) {};
    bt.loadHint = "⏎ onto track " + std::to_string(t + 1);
    bt.load = [this, t](Context& cx, const SampleBrowse::Entry& e) {
      if (e.kind == SampleBrowse::Entry::Kind::Kit) {
        cx.flash = "kits are for drum tracks";
        return;
      }
      if (e.kind != SampleBrowse::Entry::Kind::Audio) return;
      cx.session.setPadSample(t, 0, abbreviateHome(e.path.string()));
      cx.flash = "track " + std::to_string(t + 1) + " ← " + e.name;
      browsing_ = false;
    };
    return bt;
  }

  // The sample point being edited, if any (the waveform zooms around it).
  int focusPoint() const {
    const int p = kSections[static_cast<std::size_t>(section_)].rows[static_cast<std::size_t>(row_[static_cast<std::size_t>(section_)])];
    return section_ == 0 && isPoint(p) ? p : -1;
  }

  // The frames shown: the whole sample, or 1/4, 1/16, 1/64 of it round the focused point.
  std::pair<int64_t, int64_t> view(const Session& s, int t, const Sample& smp) const {
    const int64_t frames = smp.frames;
    const int64_t span = std::max<int64_t>(64, frames >> (2 * zoom_));
    const int fp = focusPoint();
    const double at = fp >= 0 ? s.param(engineParamRef(t, fp)) : s.param(engineParamRef(t, P::kStart));
    const int64_t centre = static_cast<int64_t>(at * static_cast<double>(frames - 1));
    const int64_t from = std::clamp<int64_t>(centre - span / 2, 0, std::max<int64_t>(0, frames - span));
    return {from, std::min(frames, from + span)};
  }

  void movePoint(Context& ctx, int p, int dir, bool coarse) {
    Session& s = ctx.session;
    const SampleLoader::Ptr smp = s.padSample(ctx.track, 0);
    const ParamRef ref = engineParamRef(ctx.track, p);
    if (!smp || smp->frames < 32) {
      s.nudgeParam(ref, dir, coarse);
      return;
    }
    // One column of the waveform at this zoom, ten with Shift.
    const auto [from, to] = view(s, ctx.track, *smp);
    const int64_t column = std::max<int64_t>(1, (to - from) / std::max(1, waveWidth_));
    const int64_t frames = smp->frames;
    int64_t pos = static_cast<int64_t>(std::lround(s.param(ref) * static_cast<double>(frames - 1)));
    pos = std::clamp<int64_t>(pos + dir * column * (coarse ? 10 : 1), 0, frames - 1);
    if (s.param(engineParamRef(ctx.track, P::kSnap)) >= 0.5f) {
      // The nearest rising zero crossing within a column, so loops meet without a click.
      const float* x = smp->channel(0);
      for (int64_t d = 0; d <= column; ++d) {
        if (pos + d < frames && pos + d > 0 && x[pos + d - 1] <= 0 && x[pos + d] > 0) { pos += d; break; }
        if (pos - d > 0 && x[pos - d - 1] <= 0 && x[pos - d] > 0) { pos -= d; break; }
      }
    }
    // Keep the loop at least 16 frames long and the right way round.
    const int64_t ls = static_cast<int64_t>(s.param(engineParamRef(ctx.track, P::kLoopStart)) * static_cast<double>(frames - 1));
    const int64_t le = static_cast<int64_t>(s.param(engineParamRef(ctx.track, P::kLoopEnd)) * static_cast<double>(frames - 1));
    if (p == P::kLoopStart) pos = std::min(pos, le - 16);
    if (p == P::kLoopEnd) pos = std::max(pos, ls + 16);
    pos = std::clamp<int64_t>(pos, 0, frames - 1);
    s.setParam(ref, static_cast<float>(static_cast<double>(pos) / static_cast<double>(frames - 1)));
  }

  // Peak per column of [from, to), both channels; cached until the view changes.
  const std::vector<float>& peaks(const Sample& smp, int64_t from, int64_t to, int width) {
    if (&smp != peaksFor_ || from != peaksFrom_ || to != peaksTo_ || width != static_cast<int>(peaks_.size())) {
      peaksFor_ = &smp;
      peaksFrom_ = from;
      peaksTo_ = to;
      peaks_.assign(static_cast<std::size_t>(width), 0.0f);
      for (int x = 0; x < width; ++x) {
        const int64_t a = from + (to - from) * x / width, b = std::max(a + 1, from + (to - from) * (x + 1) / width);
        float p = 0;
        for (int c = 0; c < smp.channels; ++c)
          for (int64_t i = a; i < std::min(b, smp.frames); ++i) p = std::max(p, std::fabs(smp.channel(c)[i]));
        peaks_[static_cast<std::size_t>(x)] = p;
      }
    }
    return peaks_;
  }

  const RecordingInfo& recordingInfo(const std::filesystem::path& path) {
    auto it = info_.find(path);
    if (it == info_.end()) it = info_.emplace(path, readRecordingInfo(path)).first;
    return it->second;
  }

  Element wavePane(Context& ctx, int rows) {
    Session& s = ctx.session;
    const int t = ctx.track;
    const std::string& stored = s.track(t).pads[0].sample;
    const int inner = ctx.width - 2;
    waveWidth_ = inner;
    std::string title = "Track " + std::to_string(t + 1) + " · " + s.padName(t, 0);
    Elements out;
    const SampleLoader::Ptr smp = s.padSample(t, 0);
    switch (s.padStatus(t, 0)) {
      case Session::PadStatus::Loading:
        out.push_back(text("loading " + abbreviateHome(stored)) | color(palette::faint));
        return pane(title, vbox(out), palette::border);
      case Session::PadStatus::Failed:
        out.push_back(text(fit("can't load " + abbreviateHome(stored) + ": " + s.padError(t, 0), inner)) | color(palette::record));
        out.push_back(text("o opens the browser") | color(palette::dim));
        return pane(title, vbox(out), palette::border);
      default: break;
    }
    if (!smp) return pane(title, text(""), palette::border);

    const auto [from, to] = view(s, t, *smp);
    const RecordingInfo& rec = recordingInfo(smp->path);
    const double seconds = smp->seconds();
    auto colOf = [&](double frac) {
      const double frame = frac * static_cast<double>(smp->frames - 1);
      return static_cast<int>(std::floor((frame - static_cast<double>(from)) / static_cast<double>(to - from) * inner));
    };

    // Ruler: bars and beats for a recording that knows its tempo, else seconds.
    std::string ruler(static_cast<std::size_t>(inner), ' ');
    if (rec.ok) {
      const double beat = 60.0 / rec.tempo;
      for (int b = 0; b * beat < seconds; ++b) {
        const int x = colOf(b * beat / seconds);
        if (x < 0 || x >= inner) continue;
        const bool bar = b % rec.beatsPerBar == 0;
        const std::string label = bar ? std::to_string(b / rec.beatsPerBar + 1) : "·";
        if (x + static_cast<int>(label.size()) <= inner && ruler[static_cast<std::size_t>(x)] == ' ')
          ruler.replace(static_cast<std::size_t>(x), label.size(), label);
      }
    } else {
      const double span = static_cast<double>(to - from) / kSampleRate;
      const double step = span > 20 ? 5 : span > 8 ? 1 : span > 2 ? 0.5 : span > 0.5 ? 0.1 : 0.01;
      for (double tm = std::ceil(static_cast<double>(from) / kSampleRate / step) * step; tm * kSampleRate < static_cast<double>(to); tm += step) {
        const int x = colOf(tm / seconds);
        char label[16];
        std::snprintf(label, sizeof label, step >= 1 ? "%.0fs" : step >= 0.1 ? "%.1f" : "%.2f", tm);
        const std::size_t len = std::strlen(label);
        bool free = x >= 0 && x + static_cast<int>(len) + 1 <= inner;
        for (int k = std::max(0, x - 1); free && k < std::min(inner, x + static_cast<int>(len) + 1); ++k)
          free = ruler[static_cast<std::size_t>(k)] == ' ';
        if (free) ruler.replace(static_cast<std::size_t>(x), len, label);
      }
    }
    out.push_back(text(ruler) | color(palette::faint));

    // The waveform, `rows` high, with the loop region brighter.
    const int loopMode = static_cast<int>(std::lround(s.param(engineParamRef(t, P::kLoop))));
    const int ls = colOf(s.param(engineParamRef(t, P::kLoopStart))), le = colOf(s.param(engineParamRef(t, P::kLoopEnd)));
    const auto& pk = peaks(*smp, from, to, inner);
    for (int r = rows - 1; r >= 0; --r) {
      Elements line;
      std::string run;
      bool runLoop = false;
      auto flush = [&] {
        if (!run.empty()) line.push_back(text(run) | color(runLoop ? palette::value : palette::dim));
        run.clear();
      };
      for (int x = 0; x < inner; ++x) {
        const float p = pk[static_cast<std::size_t>(x)];
        const float db = p > 0 ? 20.0f * std::log10(p) : -100.0f;
        const float level = std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f) * static_cast<float>(rows * 8);
        const int fill = std::clamp(static_cast<int>(std::lround(level)) - r * 8, 0, 8);
        const bool inLoop = loopMode != SamplerEngine::kLoopOff && x >= ls && x <= le;
        if (inLoop != runLoop) {
          flush();
          runLoop = inLoop;
        }
        run += levels[fill];
      }
      flush();
      out.push_back(hbox(line));
    }
    // Markers: ▲ start, [ ] the loop.
    std::string marks(static_cast<std::size_t>(inner), ' ');
    auto mark = [&](double frac, char c) {
      const int x = colOf(frac);
      if (x >= 0 && x < inner) marks[static_cast<std::size_t>(x)] = c;
    };
    if (loopMode != SamplerEngine::kLoopOff) {
      mark(s.param(engineParamRef(t, P::kLoopStart)), '[');
      mark(s.param(engineParamRef(t, P::kLoopEnd)), ']');
    }
    mark(s.param(engineParamRef(t, P::kStart)), '^');
    std::string shown;
    for (char c : marks) shown += c == '^' ? "▲" : std::string(1, c);
    out.push_back(text(shown) | color(palette::accent));

    std::string info;
    if (rec.ok) {
      char buf[48];
      std::snprintf(buf, sizeof buf, "%d bars · %.1f bpm · ", rec.bars, rec.tempo);
      info += buf;
    }
    char len[32];
    std::snprintf(len, sizeof len, "%.3f s", seconds);
    info += std::string(len) + " · " + rateText(smp->sourceRate) + (smp->sourceRate != 48000 ? "→48k" : "") + " " +
            (smp->channels == 1 ? "mono" : "stereo");
    if (rec.ok && !rec.sources.empty()) info += " · resampled from " + rec.sources;
    if (zoom_ > 0) info += " · zoom ×" + std::to_string(1 << (2 * zoom_));
    out.push_back(text(fit(info, inner)) | color(palette::dim));
    return pane(fit(title + " · " + abbreviateHome(stored), ctx.width - 6), vbox(out), palette::border);
  }

  std::string valueText(const Session& s, int t, int p) const {
    const ParamRef ref = engineParamRef(t, p);
    const ParamInfo& pi = SamplerEngine::paramTable()[static_cast<std::size_t>(p)];
    const float v = s.param(ref);
    if (isPoint(p)) {
      const SampleLoader::Ptr smp = s.padSample(t, 0);
      if (smp) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.3f s", static_cast<double>(v) * smp->seconds());
        return buf;
      }
    }
    if (p == P::kMode) return formatValue(pi, v);
    if (p == P::kPoly && std::lround(v) == 1) return "1 (mono)";
    return formatValue(pi, v);
  }

  bool inactive(const Session& s, int t, int p) const {
    auto v = [&](int q) { return s.param(engineParamRef(t, q)); };
    const bool loopOff = std::lround(v(P::kLoop)) == SamplerEngine::kLoopOff;
    if ((p == P::kLoopStart || p == P::kLoopEnd || p == P::kCrossfade) && loopOff) return true;
    if (p == P::kCrossfade && std::lround(v(P::kLoop)) == SamplerEngine::kLoopPingPong) return true;
    if (p != P::kFilter && kSections[2].rows.end() != std::find(kSections[2].rows.begin(), kSections[2].rows.end(), p) &&
        std::lround(v(P::kFilter)) == SamplerEngine::kFilterOff)
      return true;
    if (p == P::kLfoRate && v(P::kLfoSync) >= 0.5f) return true;
    if (p == P::kLfoDivision && v(P::kLfoSync) < 0.5f) return true;
    return false;
  }

  Element sectionPane(Context& ctx, int index, int width, int height) {
    const Session& s = ctx.session;
    const int t = ctx.track;
    const Section& sec = kSections[static_cast<std::size_t>(index)];
    const bool focused = index == section_;
    const int inner = width - 4;
    auto v = [&](int q) { return s.param(engineParamRef(t, q)); };
    Elements out;
    switch (index) {
      case 1:
      case 2: {
        const int first = index == 1 ? P::kAmpAttack : P::kFAttack;
        std::array<float, kEnvParamCount> env{};
        for (int k = 0; k < kEnvParamCount; ++k) env[static_cast<std::size_t>(k)] = v(first + k);
        out.push_back(text(envelopeShape(env.data(), inner)) | color(index == 2 && std::lround(v(P::kFilter)) == 0 ? palette::faint : palette::value));
        break;
      }
      case 3: out.push_back(text(lfoShape(static_cast<int>(std::lround(v(P::kLfoShape))), inner)) | color(palette::value)); break;
      default: {
        // Play: where the root sits in the key range.
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s–%s · root %s · stretch: later", noteName(static_cast<int>(v(P::kLowKey))).c_str(),
                      noteName(static_cast<int>(v(P::kHighKey))).c_str(), noteName(static_cast<int>(v(P::kRoot))).c_str());
        out.push_back(text(fit(buf, inner)) | color(palette::dim));
      }
    }
    const int rowsFit = std::max(3, height - 3);
    const int n = static_cast<int>(sec.rows.size());
    const int sel = row_[static_cast<std::size_t>(index)];
    const int first = std::clamp(sel - rowsFit / 2, 0, std::max(0, n - rowsFit));
    const int labelW = std::min(11, std::max(6, inner - 10));
    for (int r = first; r < std::min(n, first + rowsFit); ++r) {
      const int p = sec.rows[static_cast<std::size_t>(r)];
      const ParamInfo& pi = SamplerEngine::paramTable()[static_cast<std::size_t>(p)];
      const bool on = focused && r == sel;
      const bool dim = inactive(s, t, p);
      Element line = hbox({text(on ? "▸" : " ") | color(palette::accent),
                           text(fit(pi.label, labelW)) | size(WIDTH, EQUAL, labelW) |
                               color(on ? palette::accent : dim ? palette::faint : palette::text),
                           text(" "), text(fit(valueText(s, t, p), inner - labelW - 2)) | color(on ? palette::text : dim ? palette::faint : palette::dim),
                           filler()});
      out.push_back(ctx.value(on ? line | bgcolor(palette::raised) : line, [this, index, r](Context&) {
        section_ = index;
        row_[static_cast<std::size_t>(index)] = r;
      }));
    }
    if (index == 3) {
      // CCs mapped onto this sampler (the mockup's "cc 74 → cutoff").
      for (const Session::CcEntry& c : s.ccMappings()) {
        if (c.mapping.target.scope != t || !c.mapping.target.isEngine()) continue;
        const ParamInfo* pi = s.paramInfo(c.mapping.target);
        std::string label = pi ? pi->label : "?";
        for (char& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        out.push_back(text(fit("cc " + std::to_string(c.mapping.cc) + " → " + label, inner)) | color(palette::value));
      }
    }
    out.push_back(filler());
    std::string title = sec.title;
    if (ctx.narrow()) title += " · " + std::to_string(index + 1) + "/4 · Tab";
    return pane(title, vbox(out), ctx.piano ? palette::value : (focused ? palette::accent : palette::border));
  }

  bool browsing_ = false;
  int section_ = 0;
  std::array<int, 4> row_{};
  int zoom_ = 0;
  int waveWidth_ = 100;
  const Sample* peaksFor_ = nullptr;
  int64_t peaksFrom_ = 0, peaksTo_ = 0;
  std::vector<float> peaks_;
  std::map<std::filesystem::path, RecordingInfo> info_;
};

}  // namespace

std::unique_ptr<Screen> makeSamplerScreen() { return std::make_unique<SamplerScreen>(); }

}  // namespace ion::ui
