// Rec (R): resampling (mockups/Resample; HANDOFF §7.6). Setup on the left;
// on the right the take as it records (bars, a live waveform), the slices of
// a take bound for drum pads (nudge, add, remove, audition, commit), and the
// project's recent recordings.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "engine/drum/drum_engine.h"
#include "engine/module.h"
#include "project/paths.h"
#include "project/session.h"
#include "ui/sample_view.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;
namespace fs = std::filesystem;
using Dst = RecSetup::Dest;

enum class Row { Source, Tracks, Tap, Length, Start, Dest, DestTrack, FirstPad, Slice, After };

const int kLengths[] = {1, 2, 4, 8, 16, 0};
struct SliceChoice {
  SliceMode mode;
  int count;
  const char* label;
};
const SliceChoice kSlices[] = {{SliceMode::Chords, 0, "on chord changes"}, {SliceMode::Transients, 0, "at transients"},
                               {SliceMode::Equal, 4, "4 equal"},          {SliceMode::Equal, 8, "8 equal"},
                               {SliceMode::Equal, 16, "16 equal"}};

const char* blocks[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

int sliceIndex(const RecSetup& r) {
  for (int i = 0; i < 5; ++i)
    if (kSlices[i].mode == r.slice && (r.slice != SliceMode::Equal || kSlices[i].count == r.sliceCount)) return i;
  return 0;
}

std::string barsText(int bars) { return bars == 0 ? "until stopped" : std::to_string(bars) + (bars == 1 ? " bar" : " bars"); }

std::string padsText(int first, int n) {
  return n <= 1 ? "pad " + std::to_string(first + 1) : "pads " + std::to_string(first + 1) + "–" + std::to_string(first + n);
}

class RecScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const int body = ctx.height - 3;
    const Session::RecState st = ctx.session.recState();
    if (st != lastState_) {
      // A take starting or ending brings the Recording pane forward on a narrow screen.
      if (st != Session::RecState::Idle) pane_ = 1;
      lastState_ = st;
      if (st == Session::RecState::Review) slice_ = 0;
    }
    if (const Session::SliceReview* rv = ctx.session.review()) slice_ = std::clamp(slice_, 0, static_cast<int>(rv->slices.size()) - 1);
    if (ctx.narrow()) return pane_ == 0 ? setupPane(ctx, ctx.width, true) | flex : recordingPane(ctx, ctx.width, body) | flex;
    const int setupWidth = ctx.wide() ? 40 : 34;
    return hbox({setupPane(ctx, setupWidth, true) | size(WIDTH, EQUAL, setupWidth),
                 recordingPane(ctx, ctx.width - setupWidth, body) | flex});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    const Session::RecState st = s.recState();
    std::string error;

    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = 1 - pane_;
      return true;
    }
    if (st == Session::RecState::Review) return reviewEvent(ctx, e);

    if (e == Event::Character('r')) {
      if (st == Session::RecState::Idle) {
        if (!s.arm(error)) ctx.flash = "not recording: " + error;
      } else {
        s.stopTake();
        if (st == Session::RecState::Recording && s.take() && s.take()->synced) ctx.flash = "stopping on the next downbeat";
      }
      return true;
    }
    if (e == Event::Escape && st != Session::RecState::Idle) {
      s.cancelTake();
      return true;
    }

    const auto rows = visibleRows(s);
    row_ = std::clamp(row_, 0, static_cast<int>(rows.size()) - 1);
    if (e == Event::ArrowUp) {
      row_ = (row_ + static_cast<int>(rows.size()) - 1) % static_cast<int>(rows.size());
      return true;
    }
    if (e == Event::ArrowDown) {
      row_ = (row_ + 1) % static_cast<int>(rows.size());
      return true;
    }
    const Row row = rows[static_cast<std::size_t>(row_)];
    if (e == Event::Return && row == Row::Tracks) {
      if (!editable(st, row, ctx)) return true;
      s.recSetup().tracks ^= static_cast<uint8_t>(1 << trackCursor_);
      return true;
    }
    if (const Nudge nd = nudgeOf(e); nd.direction) {
      if (row == Row::Tracks) {
        trackCursor_ = std::clamp(trackCursor_ + nd.direction, 0, kTrackCount - 1);
        return true;
      }
      if (editable(st, row, ctx)) change(s, row, nd.direction);
      return true;
    }
    return false;
  }

  bool audition(Context& ctx) override {
    if (ctx.session.recState() != Session::RecState::Review) return false;
    ctx.session.playSlice(slice_);
    return true;
  }

  Hints hints(Context& ctx) override {
    const Session& s = ctx.session;
    Hints h;
    switch (s.recState()) {
      case Session::RecState::Review:
        h = {{"⏎", "To pads"}, {"Spc", "Play"}, {"↑↓", "Slice"}, {"←→", "Nudge"}, {"a", "Add"}, {"x", "Remove"}, {"Esc", "Keep, no pads"},
             {"u", "Undo take"}};
        break;
      case Session::RecState::Armed: h = {{"r", "Disarm"}, {"Esc", "Cancel"}, {"↑↓", "Option"}, {"←→", "Change"}}; break;
      case Session::RecState::Recording:
        h = {{"r", s.take() && s.take()->synced ? "Stop on downbeat" : "Stop"}, {"Esc", "Cancel"}, {"↑↓", "Option"}, {"←→", "Change"}};
        break;
      case Session::RecState::Idle:
        h = {{"r", "Record"}, {"↑↓", "Option"}, {"←→", "Change"}};
        if (s.recSetup().source == RecSource::Tracks) h.push_back({"⏎", "Toggle track"});
        h.push_back({"Esc", "Back"});
        break;
    }
    if (ctx.narrow()) h.insert(h.begin(), {"Tab", "Pane"});
    return h;
  }

 private:
  // ---- setup ----

  std::vector<Row> visibleRows(const Session& s) const {
    const RecSetup& r = s.recSetup();
    std::vector<Row> rows = {Row::Source};
    if (r.source == RecSource::Tracks) rows.insert(rows.end(), {Row::Tracks, Row::Tap});
    rows.insert(rows.end(), {Row::Length, Row::Start, Row::Dest});
    if (r.dest != Dst::Library) rows.push_back(Row::DestTrack);
    if (r.dest == Dst::Drum) rows.insert(rows.end(), {Row::FirstPad, Row::Slice});
    if (r.source == RecSource::Tracks) rows.push_back(Row::After);
    return rows;
  }

  // What a take has fixed can't change until it's over; where it goes still can.
  bool editable(Session::RecState st, Row row, Context& ctx) const {
    if (st == Session::RecState::Idle) return true;
    const bool fixed = row == Row::Source || row == Row::Tracks || row == Row::Tap || row == Row::Length || row == Row::Start;
    if (fixed) ctx.flash = "set once the take is over";
    return !fixed;
  }

  void change(Session& s, Row row, int dir) {
    RecSetup& r = s.recSetup();
    auto cycle = [&](int v, int n) { return ((v + dir) % n + n) % n; };
    switch (row) {
      case Row::Source: r.source = r.source == RecSource::Master ? RecSource::Tracks : RecSource::Master; break;
      case Row::Tracks: break;
      case Row::Tap: r.tap = r.tap == RecTap::Post ? RecTap::Pre : RecTap::Post; break;
      case Row::Length: {
        int i = 0;
        while (i < 5 && kLengths[i] != r.bars) ++i;
        r.bars = kLengths[std::clamp(i + dir, 0, 5)];
        break;
      }
      case Row::Start: r.synced = !r.synced; break;
      case Row::Dest: r.dest = static_cast<Dst>(cycle(static_cast<int>(r.dest), 3)); break;
      case Row::DestTrack: r.destTrack = cycle(r.destTrack, kTrackCount); break;
      case Row::FirstPad: r.firstPad = std::clamp(r.firstPad + dir, 0, DrumEngine::kPads - 1); break;
      case Row::Slice: {
        const int i = cycle(sliceIndex(r), 5);
        r.slice = kSlices[i].mode;
        if (kSlices[i].count) r.sliceCount = kSlices[i].count;
        break;
      }
      case Row::After: r.muteSources = !r.muteSources; break;
    }
  }

  // How many pads a take will fill: known for equal slices, at most what's left otherwise.
  static int padsFor(const RecSetup& r) {
    const int room = DrumEngine::kPads - r.firstPad;
    return r.slice == SliceMode::Equal ? std::min(room, r.sliceCount) : room;
  }

  std::string valueText(const Session& s, Row row) const {
    const RecSetup& r = s.recSetup();
    switch (row) {
      case Row::Source: return r.source == RecSource::Master ? "master mix" : "tracks";
      case Row::Tracks: return "";
      case Row::Tap: return r.tap == RecTap::Post ? "post-fx" : "pre-fx (dry)";
      case Row::Length: return barsText(r.bars);
      case Row::Start: return r.synced ? "next bar (clock)" : "now";
      case Row::Dest:
        switch (r.dest) {
          case Dst::Library: return "library only";
          case Dst::Sampler: return "sampler";
          case Dst::Drum: return "drum pads";
        }
        return "";
      case Row::DestTrack: {
        const int t = r.destTrack;
        const EngineType want = r.dest == Dst::Sampler ? EngineType::Sampler : EngineType::Drum;
        const EngineType has = s.track(t).engine;
        std::string v = "track " + std::to_string(t + 1);
        if (has == EngineType::None) v += " (empty)";
        else if (has != want) v += std::string(" · ") + engineLabel(has) + " → " + engineLabel(want);
        else v += " · " + trackSoundName(s, t);
        return v;
      }
      case Row::FirstPad: {
        const int n = padsFor(r);
        return r.slice == SliceMode::Equal ? padsText(r.firstPad, n) : "from pad " + std::to_string(r.firstPad + 1) + " (up to " + std::to_string(n) + ")";
      }
      case Row::Slice: return kSlices[sliceIndex(r)].label;
      case Row::After: return r.muteSources ? "mute the source tracks" : "leave the sources playing";
    }
    return "";
  }

  static const char* rowLabel(Row row) {
    switch (row) {
      case Row::Source: return "source";
      case Row::Tracks: return "tracks";
      case Row::Tap: return "tap";
      case Row::Length: return "length";
      case Row::Start: return "start";
      case Row::Dest: return "send to";
      case Row::DestTrack: return "track";
      case Row::FirstPad: return "pads";
      case Row::Slice: return "slice";
      case Row::After: return "after";
    }
    return "";
  }

  Element setupPane(Context& ctx, int width, bool) {
    const Session& s = ctx.session;
    const Session::RecState st = s.recState();
    const auto rows = visibleRows(s);
    row_ = std::clamp(row_, 0, static_cast<int>(rows.size()) - 1);
    const bool focused = st != Session::RecState::Review && (!ctx.narrow() || pane_ == 0);
    const int labelW = 8, valueW = std::max(8, width - labelW - 4);
    Elements out;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const Row row = rows[i];
      const bool on = focused && static_cast<int>(i) == row_;
      const bool locked = st != Session::RecState::Idle && st != Session::RecState::Review &&
                          (row == Row::Source || row == Row::Tracks || row == Row::Tap || row == Row::Length || row == Row::Start);
      if (row == Row::Tracks) {
        Elements cells = {text(fit(rowLabel(row), labelW)) | size(WIDTH, EQUAL, labelW) | color(on ? palette::accent : palette::text), text(" ")};
        for (int t = 0; t < kTrackCount; ++t) {
          const bool in = s.recSetup().tracks >> t & 1;
          const bool cursor = on && t == trackCursor_;
          Element c = text(std::to_string(t + 1));
          if (in) c = c | bold | color(palette::bg) | bgcolor(locked ? palette::dim : palette::record);
          else c = c | color(s.track(t).engine == EngineType::None ? palette::faint : palette::dim);
          // A click toggles the track, as ⏎ on it does.
          cells.push_back(ctx.button(hbox({text(cursor ? "[" : " ") | color(palette::accent), c, text(cursor ? "]" : " ") | color(palette::accent)}),
                                     Event::Return, [this, i, t](Context&) {
                                       pane_ = 0;
                                       row_ = static_cast<int>(i);
                                       trackCursor_ = t;
                                     }));
        }
        (void)valueW;
        Element e = hbox(cells);
        out.push_back(on ? e | bgcolor(palette::raised) : e);
        continue;
      }
      Element e = paramRow(rowLabel(row), fit(valueText(s, row), valueW), on, labelW);
      out.push_back(ctx.value(locked ? e | color(palette::faint) : e, [this, i](Context&) {
        pane_ = 0;
        row_ = static_cast<int>(i);
      }, std::nullopt));
    }
    out.push_back(filler());
    // What this setup will do, in a sentence.
    out.push_back(separatorLight() | color(palette::borderSubtle));
    for (const std::string& line : summary(ctx)) out.push_back(paragraph(line) | color(palette::dim));
    return pane("Setup", vbox(out), focused ? (ctx.piano ? palette::value : palette::accent) : palette::border);
  }

  std::vector<std::string> summary(Context& ctx) const {
    const RecSetup& r = ctx.session.recSetup();
    const ModuleStatus& ms = ctx.module.status();
    std::vector<std::string> out;
    std::string what = r.source == RecSource::Master ? "the master mix" : "the chosen tracks";
    if (r.source == RecSource::Tracks)
      what += r.tap == RecTap::Post ? " as you hear them (fader and pan, even if muted)" : ", straight from their engines";
    const std::string when = r.synced ? (ms.clockPresent.load() ? "from the next bar" : "from the next bar once the OXI's clock runs") : "at once";
    if (r.bars > 0) out.push_back("r records " + barsText(r.bars) + " of " + what + ", " + when + ".");
    else out.push_back("r records " + what + " " + when + ", until r again" + (r.synced ? " (it stops on a downbeat)." : "."));
    if (r.dest == Dst::Sampler) out.push_back("Then track " + std::to_string(r.destTrack + 1) + " plays it looped from C3.");
    if (r.dest == Dst::Drum) out.push_back("Then you check the slices and ⏎ puts them on track " + std::to_string(r.destTrack + 1) + ".");
    return out;
  }

  // ---- the recording pane ----

  Element recordingPane(Context& ctx, int width, int height) {
    Session& s = ctx.session;
    const ModuleStatus& ms = ctx.module.status();
    const Session::RecState st = s.recState();
    const Take* take = s.take();
    const int inner = std::max(10, width - 2);
    const bool hot = st == Session::RecState::Armed || st == Session::RecState::Recording;
    Elements out;

    // The big line, and what it's waiting for.
    std::string big, side;
    Color bigColor = palette::text;
    const RecSetup& r = s.recSetup();
    switch (st) {
      case Session::RecState::Idle:
        big = "READY";
        bigColor = palette::dim;
        side = r.synced ? (ms.clockPresent.load() ? "starts on the next downbeat" : "no clock yet: waits for the OXI, or set start to now")
                        : "starts when you press r";
        break;
      case Session::RecState::Armed:
        big = "◌ ARMED";
        bigColor = palette::record;
        if (ms.running.load()) big += " · starts on bar " + std::to_string(ms.bar.load() + 2);
        else side = ms.clockPresent.load() ? "waiting for the OXI to start" : "waiting for the OXI's clock";
        break;
      case Session::RecState::Recording: {
        bigColor = palette::record;
        const int64_t frames = take->frames.load(std::memory_order_acquire);
        const int target = take->targetBars.load();
        if (take->synced) {
          const int ticks = take->tickCount.load(std::memory_order_acquire);
          const int bar = std::min(target, std::max(0, ticks - 1) / 96 + 1);
          big = "● RECORDING · bar " + std::to_string(bar) + (take->bars > 0 || target < 16 ? " of " + std::to_string(target) : "");
          side = "started on bar " + std::to_string(take->startBar + 1) + " · stops on the downbeat";
        } else {
          char buf[48];
          std::snprintf(buf, sizeof buf, "● RECORDING · %.1f s", static_cast<double>(frames) / kSampleRate);
          big = buf;
          side = take->bars > 0 ? "stops after " + barsText(take->bars) : "r stops it";
        }
        break;
      }
      case Session::RecState::Review: {
        const Session::SliceReview& rv = *s.review();
        const int n = static_cast<int>(rv.slices.size());
        big = "SLICES · " + std::to_string(n) + " · ⏎ puts them on " + padsText(rv.firstPad, n);
        bigColor = palette::accent;
        side = "track " + std::to_string(rv.track + 1);
        break;
      }
    }
    out.push_back(hbox({text(big) | bold | color(bigColor), filler(), text(fit(side, std::max(0, inner - static_cast<int>(big.size()) - 2))) | color(palette::dim)}));
    out.push_back(text(""));

    // Bars: one cell each, filling as they record.
    const int bars = take ? (take->bars > 0 ? take->bars : 0) : (st == Session::RecState::Idle ? r.bars : 0);
    if (bars > 0 && st != Session::RecState::Review) {
      const int perRow = std::min(bars, 8);
      const int cellW = std::max(4, (inner - (perRow - 1)) / perRow);
      double done = 0;  // bars recorded, fractional
      if (st == Session::RecState::Recording) {
        if (take->synced) done = (take->tickCount.load() - 1) / 96.0;
        else done = static_cast<double>(take->frames.load()) / (bars * 4 * 60.0 / take->bpm * kSampleRate) * bars;
      }
      const int target = take ? take->targetBars.load() : bars;
      for (int first = 0; first < bars; first += perRow) {
        Elements labels, fills;
        for (int b = first; b < std::min(bars, first + perRow); ++b) {
          const bool current = st == Session::RecState::Recording && b == static_cast<int>(done);
          const bool dropped = b >= target;
          std::string label = "bar " + std::to_string(b + 1);
          if (r.dest == Dst::Drum && r.slice == SliceMode::Equal && r.sliceCount == bars && r.firstPad + b < DrumEngine::kPads)
            label += " → pad " + std::to_string(r.firstPad + b + 1);
          labels.push_back(text(fit(label, cellW)) | size(WIDTH, EQUAL, cellW) | color(current ? palette::record : palette::dim));
          const double f = std::clamp(done - b, 0.0, 1.0);
          const int full = static_cast<int>(std::lround(f * cellW));
          std::string fill;
          for (int i = 0; i < cellW; ++i) fill += i < full ? "█" : "░";
          fills.push_back(text(fill) | color(dropped ? palette::faint : f > 0 ? palette::record : palette::border));
          if (b + 1 < std::min(bars, first + perRow)) {
            labels.push_back(text(" "));
            fills.push_back(text(" "));
          }
        }
        out.push_back(hbox(labels));
        out.push_back(hbox(fills));
      }
      out.push_back(text(""));
    }

    // The waveform: the take so far, or the take under review with its slices.
    // Under review, a click picks the slice under the pointer and plays it.
    const int waveRows = height >= 34 ? 5 : height >= 26 ? 4 : 3;
    Element wave = waveform(ctx, inner, waveRows);
    if (st == Session::RecState::Review) {
      Hit h;
      h.open.reset();
      h.wheel = Hit::Wheel::List;  // ↑ ↓: the slices
      h.at = [this, inner](Context& c, int x, int) {
        const Session::SliceReview* rv = c.session.review();
        if (!rv || rv->slices.empty()) return;
        const int64_t frame = rv->sample->frames * std::clamp(x, 0, inner - 1) / inner;
        int i = 0;
        while (i + 1 < static_cast<int>(rv->slices.size()) && rv->slices[static_cast<std::size_t>(i + 1)] <= frame) ++i;
        slice_ = i;
        c.session.playSlice(i);
      };
      wave = ctx.hit(wave, std::move(h));
    }
    out.push_back(wave);

    // What happens to it.
    std::string what;
    if (st == Session::RecState::Review) {
      const Session::SliceReview& rv = *s.review();
      const int n = static_cast<int>(rv.slices.size());
      const int64_t from = rv.slices[static_cast<std::size_t>(slice_)];
      const int64_t to = slice_ + 1 < n ? rv.slices[static_cast<std::size_t>(slice_ + 1)] : rv.sample->frames;
      char buf[160];
      std::snprintf(buf, sizeof buf, "slice %d of %d · %s · %.3f s at %.3f s · %s", slice_ + 1, n, ("pad " + std::to_string(rv.firstPad + slice_ + 1)).c_str(),
                    static_cast<double>(to - from) / kSampleRate, static_cast<double>(from) / kSampleRate,
                    slice_ == 0 ? "the first stays at the start" : "←→ nudges it, Shift by 10 ms");
      what = buf;
    } else if (r.dest == Dst::Drum) {
      const char* how = r.slice == SliceMode::Chords ? "slices land on chord changes" : r.slice == SliceMode::Transients ? "slices land on transients" : "equal slices";
      what = std::string(how) + " · onto " + padsText(r.firstPad, padsFor(r)) + " of track " + std::to_string(r.destTrack + 1) +
             " in a free choke group · you check them first";
    } else if (r.dest == Dst::Sampler) {
      what = "goes to the sampler on track " + std::to_string(r.destTrack + 1) + " · looped · root C3";
    } else {
      what = "goes to the project's recordings (the sample browser lists them)";
    }
    out.push_back(text(fit(what, inner)) | color(palette::dim));

    // Recent recordings.
    out.push_back(separatorLight() | color(palette::borderSubtle));
    out.push_back(text("recent recordings") | color(palette::dim));
    const auto& recs = recordings(s, std::max(2, height - static_cast<int>(out.size()) - 6));
    const int lenW = 9, destW = std::min(30, std::max(12, inner / 3));
    const int nameW = std::max(8, inner - lenW - destW - 2);
    if (recs.empty()) out.push_back(text("none yet in this project") | color(palette::faint));
    for (const Session::RecordingEntry& e : recs) {
      std::string len = e.info.bars > 0 ? barsText(e.info.bars) : secondsText(e.seconds);
      out.push_back(hbox({text(fit(e.path.filename().string(), nameW)) | size(WIDTH, EQUAL, nameW), text(" "),
                          text(fit(len, lenW)) | size(WIDTH, EQUAL, lenW) | color(palette::dim), text(" "),
                          text(fit(e.info.sentTo.empty() ? "library" : e.info.sentTo, destW)) | color(palette::dim)}));
    }
    // The last take (or its slices) is undone like any edit.
    if (const std::string u = s.undoLabel(); u.rfind("take ", 0) == 0 || u.rfind("slices ", 0) == 0)
      out.push_back(text(fit("^Z undoes " + u, inner)) | color(palette::faint));

    const Color border = hot ? palette::record : st == Session::RecState::Review ? palette::accent : palette::border;
    return pane("Recording", vbox(out), border);
  }

  // Peak per column of the take (as it grows) or of the review sample.
  Element waveform(Context& ctx, int width, int rows) {
    Session& s = ctx.session;
    const Session::RecState st = s.recState();
    std::vector<float> peak(static_cast<std::size_t>(width), 0.0f);
    std::vector<int> markers;  // column of each slice
    int selFrom = -1, selTo = -1;
    int done = width;          // columns recorded so far
    if (st == Session::RecState::Review) {
      const Session::SliceReview& rv = *s.review();
      const Sample& smp = *rv.sample;
      const int64_t frames = std::max<int64_t>(1, smp.frames);
      if (cachedFor_ != &smp || cachedWidth_ != width) {
        cache_.assign(static_cast<std::size_t>(width), 0.0f);
        for (int x = 0; x < width; ++x) {
          const int64_t a = frames * x / width, b = std::max(a + 1, frames * (x + 1) / width);
          float p = 0;
          for (int c = 0; c < smp.channels; ++c)
            for (int64_t i = a; i < std::min(b, smp.frames); ++i) p = std::max(p, std::fabs(smp.channel(c)[i]));
          cache_[static_cast<std::size_t>(x)] = p;
        }
        cachedFor_ = &smp;
        cachedWidth_ = width;
      }
      peak = cache_;
      cachedTake_ = nullptr;
      for (int64_t f : rv.slices) markers.push_back(static_cast<int>(f * width / frames));
      selFrom = markers[static_cast<std::size_t>(slice_)];
      selTo = slice_ + 1 < static_cast<int>(markers.size()) ? markers[static_cast<std::size_t>(slice_ + 1)] : width;
    } else if (const Take* take = s.take(); take && st == Session::RecState::Recording) {
      // Scaled to the length it's heading for, so it fills left to right.
      const int64_t frames = take->frames.load(std::memory_order_acquire);
      int64_t total = frames;
      if (take->bars > 0) total = static_cast<int64_t>(take->targetBars.load() * 4 * 60.0 / take->bpm * kSampleRate);
      total = std::max<int64_t>({total, frames, 1});
      if (cachedTake_ != take || cachedWidth_ != width || cachedTotal_ != total) {
        cachedTake_ = take;
        cachedWidth_ = width;
        cachedTotal_ = total;
        cachedFor_ = nullptr;
        cache_.assign(static_cast<std::size_t>(width), 0.0f);
        cachedColumns_ = 0;
      }
      // Columns are computed once they're complete; the one being written each frame.
      done = static_cast<int>(std::min<int64_t>(width, frames * width / total));
      for (int x = cachedColumns_; x <= std::min(done, width - 1); ++x) {
        const int64_t a = total * x / width, b = std::min(frames, std::max(a + 1, total * (x + 1) / width));
        float p = 0;
        for (int64_t i = a; i < b; ++i)
          p = std::max({p, std::fabs(take->left[static_cast<std::size_t>(i)]), std::fabs(take->right[static_cast<std::size_t>(i)])});
        cache_[static_cast<std::size_t>(x)] = p;
      }
      cachedColumns_ = done;
      peak = cache_;
      // Bar lines.
      const int bars = take->targetBars.load();
      if (take->bars > 0)
        for (int b = 1; b < bars; ++b) markers.push_back(width * b / bars);
    } else {
      done = 0;
      cachedTake_ = nullptr;
    }

    const bool review = st == Session::RecState::Review;
    Elements lines;
    for (int row = rows - 1; row >= 0; --row) {
      Elements line;
      std::string run;
      Color runColor = palette::dim;
      auto flush = [&] {
        if (!run.empty()) line.push_back(text(run) | color(runColor));
        run.clear();
      };
      for (int x = 0; x < width; ++x) {
        const float p = peak[static_cast<std::size_t>(x)];
        const float db = p > 0 ? 20.0f * std::log10(p) : -100.0f;
        const float level = std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f) * static_cast<float>(rows * 8);
        int fill = std::clamp(static_cast<int>(std::lround(level)) - row * 8, 0, 8);
        const bool marker = std::find(markers.begin(), markers.end(), x) != markers.end() && x > 0;
        Color c = review ? (x >= selFrom && x < selTo ? palette::accent : palette::dim) : (x < done ? palette::record : palette::border);
        if (marker) {
          c = review ? palette::accent : palette::border;
          flush();
          runColor = c;
          run = "│";
          flush();
          continue;
        }
        if (x >= done && !review && row == 0 && fill == 0) fill = 1;  // the empty well still shows
        if (c != runColor) {
          flush();
          runColor = c;
        }
        run += blocks[fill];
      }
      flush();
      lines.push_back(hbox(line));
    }
    if (review) {
      // Numbers under the slices, the selected one lit.
      Elements nums;
      int col = 0;
      for (std::size_t i = 0; i < markers.size(); ++i) {
        const int x = markers[i];
        if (x < col) continue;
        if (x > col) nums.push_back(text(std::string(static_cast<std::size_t>(x - col), ' ')));
        const std::string label = std::to_string(i + 1);
        nums.push_back(text(label) | bold | color(static_cast<int>(i) == slice_ ? palette::accent : palette::dim));
        col = x + static_cast<int>(label.size());
      }
      lines.push_back(hbox(nums));
    }
    if (st == Session::RecState::Idle || st == Session::RecState::Armed) {
      lines.push_back(text(st == Session::RecState::Armed ? "the take draws here as it records" : "") | color(palette::faint));
    }
    return vbox(lines) | bgcolor(palette::surface);
  }

  bool reviewEvent(Context& ctx, const Event& e) {
    Session& s = ctx.session;
    const Session::SliceReview& rv = *s.review();
    const int n = static_cast<int>(rv.slices.size());
    slice_ = std::clamp(slice_, 0, n - 1);
    std::string error;
    if (e == Event::ArrowUp || e == Event::Character('k')) {
      slice_ = (slice_ + n - 1) % n;
      s.playSlice(slice_);
      return true;
    }
    if (e == Event::ArrowDown || e == Event::Character('j')) {
      slice_ = (slice_ + 1) % n;
      s.playSlice(slice_);
      return true;
    }
    if (const Nudge nd = nudgeOf(e); nd.direction) {
      if (slice_ == 0) {
        ctx.flash = "the first slice starts with the take";
        return true;
      }
      s.moveSlice(slice_, nd.direction * (nd.coarse ? 480 : 48));  // 1 ms, or 10 ms
      return true;
    }
    if (e == Event::Character('a')) {
      const int64_t from = rv.slices[static_cast<std::size_t>(slice_)];
      const int64_t to = slice_ + 1 < n ? rv.slices[static_cast<std::size_t>(slice_ + 1)] : rv.sample->frames;
      const int at = s.addSlice((from + to) / 2);
      if (at < 0) ctx.flash = n >= rv.maxSlices() ? "no more pads: " + std::to_string(rv.maxSlices()) + " slices at most from pad " + std::to_string(rv.firstPad + 1) : "too short to split";
      else slice_ = at;
      return true;
    }
    if (e == Event::Character('x')) {
      if (slice_ == 0) ctx.flash = "the first slice can't go (it's where the take starts)";
      else {
        s.removeSlice(slice_);
        slice_ = std::max(0, slice_ - 1);
      }
      return true;
    }
    if (e == Event::Return) {
      if (!s.commitSlices(error)) ctx.flash = "slices not saved: " + error;
      slice_ = 0;
      return true;
    }
    if (e == Event::Escape) {
      s.keepRecording();
      return true;
    }
    if (e == Event::Character('r')) {
      ctx.flash = "⏎ puts these slices on pads, or Esc keeps just the recording, before the next take";
      return true;
    }
    return false;
  }

  // The project's recordings, re-read when the folder changes.
  const std::vector<Session::RecordingEntry>& recordings(const Session& s, int max) {
    std::error_code ec;
    const fs::path dir = s.recordingsDir();
    const auto stamp = fs::last_write_time(dir, ec);
    if (dir != recDir_ || stamp != recStamp_ || ec || max != recMax_) {
      recDir_ = dir;
      recStamp_ = stamp;
      recMax_ = max;
      recs_ = s.recordings(max);
    }
    return recs_;
  }

  int pane_ = 0;  // narrow: 0 setup, 1 recording
  int row_ = 0;
  int trackCursor_ = 0;
  int slice_ = 0;
  Session::RecState lastState_ = Session::RecState::Idle;

  std::vector<float> cache_;
  const Sample* cachedFor_ = nullptr;
  const Take* cachedTake_ = nullptr;
  int cachedWidth_ = 0, cachedColumns_ = 0;
  int64_t cachedTotal_ = 0;

  fs::path recDir_;
  fs::file_time_type recStamp_{};
  int recMax_ = 0;
  std::vector<Session::RecordingEntry> recs_;
};

}  // namespace

std::unique_ptr<Screen> makeRecScreen() { return std::make_unique<RecScreen>(); }

}  // namespace ion::ui
