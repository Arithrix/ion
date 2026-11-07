// Instrument (I) for a DRUM track (mockups/DrumKit; HANDOFF §6.1): the sample
// browser on the left, the 4×4 pads, and the selected pad's settings.
// Moving through the browser previews each file; ⏎ puts it on the selected pad.

#include <chrono>
#include <cmath>
#include <cstdio>

#include "engine/drum/drum_engine.h"
#include "engine/module.h"
#include "project/paths.h"
#include "project/session.h"
#include "ui/sample_browse.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;
using Clock = std::chrono::steady_clock;

// The pad editor's rows, in two columns: playback and envelope on the left,
// tone and response on the right.
constexpr int kRows[] = {DrumEngine::kTune,   DrumEngine::kFine,     DrumEngine::kLevel,     DrumEngine::kPan,
                         DrumEngine::kStart,  DrumEngine::kReverse,  DrumEngine::kMode,      DrumEngine::kHold,
                         DrumEngine::kDecay,  DrumEngine::kFilter,   DrumEngine::kCutoff,    DrumEngine::kReso,
                         DrumEngine::kDrive,  DrumEngine::kVelLevel, DrumEngine::kVelFilter, DrumEngine::kChoke,
                         DrumEngine::kNote};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kLeftRows = 9;

Color chokeColor(int group) {
  switch (group) {
    case 1: return palette::accent;
    case 2: return palette::lilac;
    case 3: return palette::green;
    case 4: return palette::value;
    default: return palette::faint;
  }
}

std::string secondsText(double s) {
  char buf[32];
  if (s < 10) std::snprintf(buf, sizeof buf, "%.2f s", s);
  else if (s < 60) std::snprintf(buf, sizeof buf, "%.1f s", s);
  else std::snprintf(buf, sizeof buf, "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
  return buf;
}

std::string rateText(unsigned rate) {
  char buf[16];
  if (rate % 1000 == 0) std::snprintf(buf, sizeof buf, "%uk", rate / 1000);
  else std::snprintf(buf, sizeof buf, "%.1fk", rate / 1000.0);
  return buf;
}

// The sample's level over its length, one block character per column.
std::string waveform(const Sample& s, int width) {
  static const char* levels[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  std::string out;
  for (int x = 0; x < width; ++x) {
    const int from = x * Sample::kOverview / width, to = std::max(from + 1, (x + 1) * Sample::kOverview / width);
    float p = 0;
    for (int i = from; i < to; ++i) p = std::max(p, s.overview[static_cast<std::size_t>(i)]);
    // A dB-ish scale, so tails show: −48 dB is the bottom.
    const float db = p > 0 ? 20.0f * std::log10(p) : -100.0f;
    const int n = std::clamp(static_cast<int>(std::lround((db + 48.0f) / 48.0f * 8.0f)), p > 0 ? 1 : 0, 8);
    out += levels[n];
  }
  return out;
}

// A bar centred on zero for −1…+1.
Element centredBar(float v, int width, bool focused) {
  const int half = width / 2;
  const int n = static_cast<int>(std::lround(std::min(1.0f, std::fabs(v)) * static_cast<float>(half)));
  std::string left, right;
  for (int i = 0; i < half; ++i) left += (v < 0 && i >= half - n) ? "█" : "░";
  for (int i = 0; i < width - half; ++i) right += (v > 0 && i < n) ? "█" : "░";
  const Color on = focused ? palette::accent : palette::value;
  return hbox({text(left) | color(v < 0 ? on : palette::border), text(right) | color(v > 0 ? on : palette::border)});
}

class DrumScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    trackHits(ctx);
    const int body = ctx.height - 3;  // header, status line, hint bar
    if (ctx.narrow()) {
      // CometFM-style: the browser on its own, or the pads over the pad editor.
      if (pane_ == 0) return browser(ctx, ctx.width, body) | flex;
      const bool compact = body < 32;
      const int padsHeight = compact ? 10 : 17;
      return vbox({pads(ctx, ctx.width, compact) | size(HEIGHT, EQUAL, padsHeight), params(ctx, ctx.width, body - padsHeight) | flex}) | flex;
    }
    const int left = std::clamp(ctx.width * 36 / 100, 30, 52);
    const int right = ctx.width - left;
    const bool compact = body < 32;
    const int padsHeight = compact ? 10 : 17;
    return hbox({browser(ctx, left, body) | size(WIDTH, EQUAL, left),
                 vbox({pads(ctx, right, compact) | size(HEIGHT, EQUAL, padsHeight), params(ctx, right, body - padsHeight) | flex}) | flex});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    SampleBrowse& b = *ctx.samples;
    const int t = ctx.track;
    int& pad = ctx.pad[static_cast<std::size_t>(t)];

    if (b.typing) {
      if (e == Event::Return) { b.typing = false; return true; }
      if (e == Event::Escape) { b.typing = false; b.setFilter({}); return true; }
      if (e == Event::Backspace) {
        std::string f = b.filter;
        if (!f.empty()) f.pop_back();
        b.setFilter(f);
        return true;
      }
      if (e.is_character()) { b.setFilter(b.filter + e.character()); return true; }
      if (e == Event::ArrowUp || e == Event::ArrowDown) b.typing = false;  // and fall through to move
    }

    if (e == Event::Tab) { pane_ = (pane_ + 1) % 3; return true; }
    if (e == Event::TabReverse) { pane_ = (pane_ + 2) % 3; return true; }

    // Keys that work in every pane.
    if (e == Event::Character('c')) {
      const ParamRef ref = engineParamRef(t, DrumEngine::param(pad, DrumEngine::kChoke));
      s.setParam(ref, std::fmod(s.param(ref) + 1.0f, 5.0f));
      const ParamInfo* pi = s.paramInfo(ref);
      ctx.flash = "pad " + std::to_string(pad + 1) + " choke group: " + (pi ? formatValue(*pi, s.param(ref)) : "?");
      return true;
    }
    if (e == Event::Character('x')) {
      if (!s.track(t).pads[static_cast<std::size_t>(pad)].sample.empty()) {
        s.setPadSample(t, pad, "");
        ctx.flash = "pad " + std::to_string(pad + 1) + " cleared";
      }
      return true;
    }
    if (e == Event::Character('k')) { startSaveKit(ctx); return true; }
    if (e == Event::Character('g')) { collect(ctx); return true; }

    if (pane_ == 0) return browserEvent(ctx, e);
    if (pane_ == 1) {
      int to = pad;
      if (e == Event::ArrowLeft) to = pad % 4 == 0 ? pad : pad - 1;
      else if (e == Event::ArrowRight) to = pad % 4 == 3 ? pad : pad + 1;
      else if (e == Event::ArrowUp) to = pad < 4 ? pad : pad - 4;
      else if (e == Event::ArrowDown) to = pad >= 12 ? pad : pad + 4;
      else if (e == Event::Return) { ctx.request = Context::Request::Audition; return true; }
      else return false;
      pad = to;
      return true;
    }
    // The pad editor.
    if (e == Event::ArrowUp) { row_ = (row_ + kRowCount - 1) % kRowCount; return true; }
    if (e == Event::ArrowDown) { row_ = (row_ + 1) % kRowCount; return true; }
    if (e == Event::Return) { ctx.request = Context::Request::Audition; return true; }
    if (const Nudge n = nudgeOf(e); n.direction) {
      s.nudgeParam(engineParamRef(t, DrumEngine::param(pad, kRows[row_])), n.direction, n.coarse);
      return true;
    }
    return false;
  }

  Hints hints(Context& ctx) override {
    if (ctx.samples->typing) return {{"type", "Filter"}, {"⏎", "Done"}, {"Esc", "Clear"}, {"↑↓", "Browse"}};
    Hints h;
    if (pane_ == 0) h = {{"↑↓", "Browse"}, {"⏎", "Load"}, {"←", "Up"}, {"/", "Filter"}, {"Spc", "Pad"}};
    else if (pane_ == 1) h = {{"←→↑↓", "Pad"}, {"⏎", "Play"}, {"c", "Choke"}, {"x", "Clear"}};
    else h = {{"↑↓", "Param"}, {"←→", "Adjust"}, {"l", "Learn"}, {"⏎", "Play"}, {"c", "Choke"}};
    h.push_back({"Tab", "Pane"});
    if (!ctx.narrow()) {
      h.push_back({"k", "Save kit"});
      h.push_back({"g", "Collect"});
    }
    h.push_back({"Esc", "Back"});
    return h;
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    if (pane_ != 2) return std::nullopt;
    return engineParamRef(ctx.track, DrumEngine::param(ctx.pad[static_cast<std::size_t>(ctx.track)], kRows[row_]));
  }

  bool capturesText(Context& ctx) override { return ctx.samples->typing; }

 private:
  // ---- the browser ----

  bool browserEvent(Context& ctx, const Event& e) {
    Session& s = ctx.session;
    SampleBrowse& b = *ctx.samples;
    const int visible = std::max(3, ctx.height - 12);
    int delta = 0;
    if (e == Event::ArrowUp) delta = -1;
    else if (e == Event::ArrowDown) delta = 1;
    else if (e == Event::PageUp) delta = -visible;
    else if (e == Event::PageDown) delta = visible;
    else if (e == Event::Home) delta = -100000;
    else if (e == Event::End) delta = 100000;
    if (delta) {
      const int before = b.cursor(s);
      b.move(s, delta);
      if (b.cursor(s) != before) moved(ctx);
      return true;
    }
    if (e == Event::Character('/')) { b.typing = true; return true; }
    if (e == Event::Escape && !b.filter.empty()) { b.setFilter({}); return true; }
    if (e == Event::ArrowLeft || e == Event::Backspace) {
      b.up(s);
      return true;
    }
    const SampleBrowse::Entry* cur = b.current(s);
    if (!cur) return false;
    if (e == Event::ArrowRight || e == Event::Return) {
      if (b.open(s, *cur)) return true;
      if (e != Event::Return) return true;
      const int t = ctx.track, pad = ctx.pad[static_cast<std::size_t>(t)];
      if (cur->kind == SampleBrowse::Entry::Kind::Audio) {
        s.setPadSample(t, pad, abbreviateHome(cur->path.string()));
        ctx.flash = "pad " + std::to_string(pad + 1) + " ← " + cur->name;
      } else if (cur->kind == SampleBrowse::Entry::Kind::Kit) {
        std::string error;
        if (s.loadKit(t, cur->path, error)) ctx.flash = "kit " + s.track(t).kit + " loaded onto track " + std::to_string(t + 1);
        else ctx.flash = "kit not loaded: " + error;
      }
      return true;
    }
    return false;
  }

  void moved(Context& ctx) {
    Session& s = ctx.session;
    const SampleBrowse::Entry* cur = ctx.samples->current(s);
    if (cur && cur->kind == SampleBrowse::Entry::Kind::Audio) s.preview(cur->path, s.config().samplePreview);
  }

  Element browser(Context& ctx, int width, int height) {
    Session& s = ctx.session;
    SampleBrowse& b = *ctx.samples;
    const bool focused = pane_ == 0;
    const auto& list = b.entries(s);
    const int inner = width - 4;
    Elements rows;
    // The end of the path matters most: "…/garage/kicks/".
    std::string where = b.location();
    if (static_cast<int>(where.size()) > inner) where = "…" + where.substr(where.size() - static_cast<std::size_t>(inner - 1));
    rows.push_back(text(where) | color(palette::dim));
    if (b.typing)
      rows.push_back(hbox({text("/ ") | color(palette::accent), text(b.filter), text(" ") | bgcolor(palette::accent)}));
    else if (!b.filter.empty())
      rows.push_back(hbox({text("/ ") | color(palette::accent), text(b.filter), text("  Esc clears") | color(palette::faint)}));
    else
      rows.push_back(text("/ filter") | color(palette::faint));

    const int previewLines = 4;
    const int visible = std::max(3, height - 2 - 2 - previewLines);
    const int cur = b.cursor(s);
    const int n = static_cast<int>(list.size());
    const int first = std::clamp(cur - visible / 2, 0, std::max(0, n - visible));
    for (int i = first; i < std::min(n, first + visible); ++i) {
      const SampleBrowse::Entry& e = list[static_cast<std::size_t>(i)];
      const bool on = i == cur;
      std::string right;
      Color c = palette::text;
      switch (e.kind) {
        case SampleBrowse::Entry::Kind::Root:
          c = e.missing ? palette::faint : palette::value;
          if (e.missing) right = "not found";
          break;
        case SampleBrowse::Entry::Kind::Parent: c = palette::dim; break;
        case SampleBrowse::Entry::Kind::Folder: c = palette::value; break;
        case SampleBrowse::Entry::Kind::Kit: c = palette::lilac; right = "kit"; break;
        case SampleBrowse::Entry::Kind::Audio: {
          const AudioFileInfo& info = b.info(e.path);
          right = info.ok ? secondsText(info.seconds) : "?";
          break;
        }
      }
      const int nameW = std::max(4, inner - static_cast<int>(right.size()) - 1);
      Element row = hbox({text(fit(e.name, nameW)) | color(on ? palette::bg : c), filler(),
                          text(right) | color(on ? palette::bg : palette::dim)});
      if (on) row = row | bgcolor(focused ? palette::accent : palette::raised);
      rows.push_back(row);
    }
    if (list.empty() || (n == 1 && list[0].kind == SampleBrowse::Entry::Kind::Parent))
      rows.push_back(text(b.filter.empty() ? "  no folders or audio files here" : "  nothing matches") | color(palette::faint));
    rows.push_back(filler());

    // The highlighted file.
    rows.push_back(separatorLight() | color(palette::borderSubtle));
    const SampleBrowse::Entry* e = b.current(s);
    const int pad = ctx.pad[static_cast<std::size_t>(ctx.track)];
    if (e && e->kind == SampleBrowse::Entry::Kind::Audio) {
      const SampleLoader::Ptr smp = s.previewed(e->path);
      const std::string err = s.previewError(e->path);
      rows.push_back(text(fit("preview · " + e->name, inner)) | color(palette::dim));
      if (smp) rows.push_back(text(waveform(*smp, inner)) | color(palette::value));
      else rows.push_back(text(err.empty() ? (s.config().samplePreview ? "loading…" : "") : "can't load: " + err) |
                          color(err.empty() ? palette::faint : palette::record));
      const AudioFileInfo& info = b.info(e->path);
      std::string line = info.ok ? secondsText(info.seconds) + " · " + rateText(info.rate) + " · " +
                                       (info.channels == 1 ? "mono" : info.channels == 2 ? "stereo" : std::to_string(info.channels) + " ch")
                                 : "unreadable";
      line += " · ⏎ onto pad " + std::to_string(pad + 1);
      rows.push_back(text(fit(line, inner)) | color(palette::dim));
    } else if (e && e->kind == SampleBrowse::Entry::Kind::Kit) {
      rows.push_back(text("kit · " + e->name) | color(palette::lilac));
      rows.push_back(text("⏎ loads all 16 pads onto track " + std::to_string(ctx.track + 1)) | color(palette::dim));
      rows.push_back(text("its samples stay where they are") | color(palette::faint));
    } else {
      rows.push_back(text(b.atTop() ? "sample roots: Settings (,)" : "") | color(palette::faint));
      rows.push_back(text(s.config().samplePreview ? "preview on move ● on" : "preview on move ○ off") | color(palette::faint));
      rows.push_back(text("⏎ opens · ← goes up") | color(palette::faint));
    }
    std::string title = "Samples";
    if (ctx.narrow()) title += " · Tab ⇄";
    const int loading = s.samplesLoading();
    if (loading > 0) title += " · loading " + std::to_string(loading);
    return pane(title, vbox(rows), ctx.piano ? palette::value : (focused ? palette::accent : palette::border));
  }

  // ---- the pads ----

  void trackHits(Context& ctx) {
    const auto now = Clock::now();
    for (std::size_t t = 0; t < kTrackCount; ++t)
      for (std::size_t p = 0; p < 16; ++p) {
        const uint32_t h = ctx.module.status().padHits[t][p].load(std::memory_order_relaxed);
        if (h != lastHits_[t][p]) {
          lastHits_[t][p] = h;
          hitAt_[t][p] = t == static_cast<std::size_t>(ctx.track) ? now : Clock::time_point{};
        }
      }
  }

  // 0…1: how lit a pad's activity light is (fades over 400 ms).
  float activity(int t, int p) const {
    const auto age = Clock::now() - hitAt_[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)];
    const float ms = std::chrono::duration<float, std::milli>(age).count();
    return std::clamp(1.0f - ms / 400.0f, 0.0f, 1.0f);
  }

  Element padCell(Context& ctx, int p, int width, bool compact) {
    const Session& s = ctx.session;
    const int t = ctx.track;
    const bool selected = p == ctx.pad[static_cast<std::size_t>(t)];
    const bool focused = pane_ == 1 && selected;
    const int group = static_cast<int>(std::lround(s.param(engineParamRef(t, DrumEngine::param(p, DrumEngine::kChoke)))));
    const int note = static_cast<int>(std::lround(s.param(engineParamRef(t, DrumEngine::param(p, DrumEngine::kNote)))));
    const float lit = activity(t, p);

    char num[4];
    std::snprintf(num, sizeof num, "%02d", p + 1);
    Element number = text(num);
    if (focused) number = number | color(palette::bg) | bgcolor(palette::accent);
    else if (compact && lit > 0.5f) number = number | color(palette::bg) | bgcolor(palette::value);
    else number = number | color(selected ? palette::accent : palette::dim);
    Element line1 = hbox({number, text(" " + noteName(note)) | color(palette::faint), filler(),
                          text(group ? std::string("grp ") + "ABCD"[group - 1] : "") | color(chokeColor(group))});

    std::string name;
    Color nameColor = selected ? palette::accent : palette::text;
    switch (s.padStatus(t, p)) {
      case Session::PadStatus::Empty: name = "—"; nameColor = palette::faint; break;
      case Session::PadStatus::Loading: name = "loading…"; nameColor = palette::faint; break;
      case Session::PadStatus::Failed: name = "missing " + s.padName(t, p); nameColor = palette::record; break;
      case Session::PadStatus::Ready: name = s.padName(t, p); break;
    }
    Elements lines = {line1, text(fit(name, width)) | color(nameColor)};
    if (!compact) {
      const int n = static_cast<int>(std::lround(lit * 7));
      std::string on, off;
      for (int i = 0; i < 7; ++i) (i < n ? on : off) += i < n ? "▮" : "▯";
      lines.push_back(hbox({text(on) | color(palette::value), text(off) | color(palette::borderSubtle)}));
    }
    Element cell = vbox(lines) | size(WIDTH, EQUAL, width);
    return selected ? cell | bgcolor(palette::raised) : cell;
  }

  Element pads(Context& ctx, int width, bool compact) {
    const Session& s = ctx.session;
    const TrackState& tr = s.track(ctx.track);
    const int cellW = std::max(6, (width - 2 - 3) / 4);
    Elements grid;
    for (int r = 0; r < 4; ++r) {
      Elements cells;
      for (int c = 0; c < 4; ++c) {
        if (c) cells.push_back(text(" "));
        cells.push_back(padCell(ctx, r * 4 + c, cellW, compact));
      }
      if (r && !compact) grid.push_back(text(""));
      grid.push_back(hbox(cells));
    }
    grid.push_back(filler());
    std::string title = "Track " + std::to_string(ctx.track + 1) + " · " + (tr.kit.empty() ? std::string("kit not saved") : tr.kit);
    if (ctx.narrow()) title = (tr.kit.empty() ? std::string("Pads") : tr.kit) + " · Tab ⇄";
    return pane(title, vbox(grid), ctx.piano ? palette::value : (pane_ == 1 ? palette::accent : palette::border));
  }

  // ---- the pad editor ----

  Element paramCell(Context& ctx, int row, int width) {
    const Session& s = ctx.session;
    const int t = ctx.track, pad = ctx.pad[static_cast<std::size_t>(t)];
    const int p = kRows[row];
    const ParamRef ref = engineParamRef(t, DrumEngine::param(pad, p));
    const ParamInfo& pi = DrumEngine::padParamTable()[static_cast<std::size_t>(p)];
    const float v = s.param(ref);
    const bool on = pane_ == 2 && row == row_;
    const int labelW = width >= 34 ? 12 : 10;
    const int valueW = 9;
    const int barW = width - 2 - labelW - 1 - valueW - 2;

    std::string value = formatValue(pi, v);
    bool inactive = false;
    const auto padValue = [&](int q) { return s.param(engineParamRef(t, DrumEngine::param(pad, q))); };
    if (p == DrumEngine::kHold && std::lround(padValue(DrumEngine::kMode)) == DrumEngine::kGate) {
      value = "gate";
      inactive = true;
    }
    if ((p == DrumEngine::kCutoff || p == DrumEngine::kReso || p == DrumEngine::kVelFilter) &&
        std::lround(padValue(DrumEngine::kFilter)) == DrumEngine::kFilterOff)
      inactive = true;

    Element bar = text("");
    if (barW >= 4) {
      if (pi.curve == Curve::Toggle || pi.curve == Curve::Choice) {
        if (p == DrumEngine::kTune || p == DrumEngine::kFine) bar = centredBar(v / pi.max, barW, on);
        else bar = text(std::string(static_cast<std::size_t>(barW), ' '));
      } else if (p == DrumEngine::kPan) {
        bar = centredBar(v, barW, on);
      } else {
        bar = valueBar(toNormalized(pi, v), barW, inactive ? palette::faint : palette::value);
      }
    }
    Element row_e = hbox({text(on ? "▸ " : "  ") | color(palette::accent),
                          text(fit(pi.label, labelW)) | size(WIDTH, EQUAL, labelW) | color(on ? palette::accent : inactive ? palette::faint : palette::text),
                          text(" "), bar | size(WIDTH, EQUAL, std::max(0, barW)), text(" "),
                          text(value) | color(on ? palette::text : inactive ? palette::faint : palette::dim), filler()});
    return on ? row_e | bgcolor(palette::raised) : row_e;
  }

  Element params(Context& ctx, int width, int height) {
    const Session& s = ctx.session;
    const int t = ctx.track, pad = ctx.pad[static_cast<std::size_t>(t)];
    const int inner = width - 4;
    Elements out;
    const std::string& stored = s.track(t).pads[static_cast<std::size_t>(pad)].sample;
    const SampleLoader::Ptr smp = s.padSample(t, pad);
    switch (s.padStatus(t, pad)) {
      case Session::PadStatus::Empty:
        out.push_back(text("empty · ⏎ on a file in the browser loads it here") | color(palette::faint));
        break;
      case Session::PadStatus::Loading: out.push_back(text("loading " + fit(abbreviateHome(stored), inner - 8)) | color(palette::faint)); break;
      case Session::PadStatus::Failed:
        out.push_back(text(fit("can't load " + abbreviateHome(stored) + ": " + s.padError(t, pad), inner)) | color(palette::record));
        break;
      case Session::PadStatus::Ready: {
        std::string info = secondsText(smp->seconds()) + " · " + rateText(smp->sourceRate) +
                           (smp->sourceRate != 48000 ? "→48k" : "") + " · " + (smp->channels == 1 ? "mono" : "stereo") + " · " +
                           fit(abbreviateHome(stored), std::max(8, inner - 30));
        out.push_back(text(fit(info, inner)) | color(palette::dim));
        break;
      }
    }
    // Short: the info line goes (the title still names the sample); roomy: a waveform too.
    if (height < 12) out.clear();
    const bool roomy = height >= 14;
    if (roomy) out.push_back(smp ? text(waveform(*smp, inner)) | color(palette::value) : text(""));

    const bool twoColumns = width >= 44;
    const int colW = twoColumns ? (inner - 2) / 2 : inner;
    Elements left, right;
    for (int r = 0; r < kRowCount; ++r) (twoColumns && r >= kLeftRows ? right : left).push_back(paramCell(ctx, r, colW));
    if (twoColumns) out.push_back(hbox({vbox(left) | size(WIDTH, EQUAL, colW), text("  "), vbox(right) | size(WIDTH, EQUAL, colW)}));
    else {
      // One column: keep the focused row in view.
      const int visible = std::max(3, height - 4 - (roomy ? 1 : 0));
      const int first = std::clamp(row_ - visible / 2, 0, std::max(0, kRowCount - visible));
      for (int r = first; r < std::min(kRowCount, first + visible); ++r) out.push_back(left[static_cast<std::size_t>(r)]);
    }
    out.push_back(filler());

    // CCs mapped to this pad.
    std::string ccs;
    for (const Session::CcEntry& c : s.ccMappings()) {
      const ParamRef& tgt = c.mapping.target;
      if (tgt.scope != t || !tgt.isEngine()) continue;
      const int index = tgt.index - kEngineParamBase;
      if (index / DrumEngine::kPadParamCount != pad) continue;
      std::string label = DrumEngine::padParamTable()[static_cast<std::size_t>(index % DrumEngine::kPadParamCount)].label;
      for (char& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      ccs += (ccs.empty() ? "" : " · ") + std::string("cc ") + std::to_string(c.mapping.cc) + " → " + label;
    }
    if (!ccs.empty() && height >= 12) out.push_back(text(fit(ccs, inner)) | color(palette::value));

    std::string title = "Pad " + std::to_string(pad + 1);
    if (!stored.empty()) title += " · " + s.padName(t, pad);
    return pane(fit(title, width - 6), vbox(out), ctx.piano ? palette::value : (pane_ == 2 ? palette::accent : palette::border));
  }

  // ---- actions ----

  void startSaveKit(Context& ctx) {
    const int t = ctx.track;
    const TrackState& tr = ctx.session.track(t);
    ctx.promptTitle = "Save kit as (in ~/ion/kits; the same name replaces it)";
    ctx.promptInitial = tr.kit.empty() ? ctx.session.name() + " kit" : tr.kit;
    ctx.promptDone = [t](Context& cx, const std::string& name) {
      const bool existed = std::filesystem::exists(cx.session.paths().kitFile(name));
      std::string error;
      if (cx.session.saveKit(t, name, error)) cx.flash = (existed ? "kit replaced: " : "kit saved: ") + name;
      else cx.flash = "kit not saved: " + error;
    };
    ctx.request = Context::Request::Prompt;
  }

  void collect(Context& ctx) {
    std::string error;
    const int n = ctx.session.collectSamples(error);
    if (n < 0) {
      ctx.flash = "not collected: " + error;
      return;
    }
    std::string msg = n == 0 ? "nothing to collect: every sample is already in the project"
                             : "copied " + std::to_string(n) + " sample" + (n == 1 ? "" : "s") + " into the project's samples/ · w saves";
    if (!error.empty()) msg += " · " + error;
    ctx.flash = msg;
  }

  int pane_ = 0;
  int row_ = 0;
  std::array<std::array<uint32_t, 16>, kTrackCount> lastHits_{};
  std::array<std::array<Clock::time_point, 16>, kTrackCount> hitAt_{};
};

}  // namespace

std::unique_ptr<Screen> makeDrumScreen() { return std::make_unique<DrumScreen>(); }

}  // namespace ion::ui
