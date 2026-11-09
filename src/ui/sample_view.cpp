#include "ui/sample_view.h"

#include <cmath>
#include <cstdio>

#include "engine/module.h"
#include "project/paths.h"
#include "project/session.h"

namespace ion::ui {

using namespace ftxui;

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

std::string waveformText(const Sample& s, int width) {
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

namespace {

// Moving onto a file previews it; moving off one stops the preview (a long
// loop shouldn't keep playing under the next thing).
void moved(Context& ctx) {
  Session& s = ctx.session;
  const SampleBrowse::Entry* cur = ctx.samples->current(s);
  if (cur && cur->kind == SampleBrowse::Entry::Kind::Audio) s.preview(cur->path, s.config().samplePreview);
  else if (ctx.module.status().previewing.load(std::memory_order_relaxed)) s.stopPreview();
}

}  // namespace

bool sampleBrowserEvent(Context& ctx, const Event& e, const BrowserTarget& target) {
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
    moved(ctx);
    return true;
  }
  const SampleBrowse::Entry* cur = b.current(s);
  if (!cur) return false;
  if (e == Event::ArrowRight || e == Event::Return) {
    if (b.open(s, *cur)) {
      moved(ctx);
      return true;
    }
    if (e == Event::Return && target.load) target.load(ctx, *cur);
    return true;
  }
  return false;
}

Element sampleBrowserPane(Context& ctx, int width, int height, bool focused, const BrowserTarget& target) {
  Session& s = ctx.session;
  SampleBrowse& b = *ctx.samples;
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
    // Click moves there (previewing, as ↑ ↓ do); double-click opens or loads, as ⏎.
    rows.push_back(ctx.item(row, [i, focus = target.focus](Context& cx) {
      if (focus) focus(cx);
      SampleBrowse& sb = *cx.samples;
      const int at = sb.cursor(cx.session);
      if (at == i) return;
      sb.move(cx.session, i - at);
      moved(cx);
    }, target.focus));
  }
  if (list.empty() || (n == 1 && list[0].kind == SampleBrowse::Entry::Kind::Parent))
    rows.push_back(text(b.filter.empty() ? "  no folders or audio files here" : "  nothing matches") | color(palette::faint));
  rows.push_back(filler());

  // The highlighted file.
  rows.push_back(separatorLight() | color(palette::borderSubtle));
  const SampleBrowse::Entry* e = b.current(s);
  if (e && e->kind == SampleBrowse::Entry::Kind::Audio) {
    const SampleLoader::Ptr smp = s.previewed(e->path);
    const std::string err = s.previewError(e->path);
    rows.push_back(text(fit("preview · " + e->name, inner)) | color(palette::dim));
    if (smp) rows.push_back(text(waveformText(*smp, inner)) | color(palette::value));
    else rows.push_back(text(err.empty() ? (s.config().samplePreview ? "loading…" : "") : "can't load: " + err) |
                        color(err.empty() ? palette::faint : palette::record));
    const AudioFileInfo& info = b.info(e->path);
    std::string line = info.ok ? secondsText(info.seconds) + " · " + rateText(info.rate) + " · " +
                                     (info.channels == 1 ? "mono" : info.channels == 2 ? "stereo" : std::to_string(info.channels) + " ch")
                               : "unreadable";
    line += " · " + target.loadHint;
    rows.push_back(text(fit(line, inner)) | color(palette::dim));
  } else if (e && e->kind == SampleBrowse::Entry::Kind::Kit) {
    rows.push_back(text("kit · " + e->name) | color(palette::lilac));
    rows.push_back(text(target.kitHint) | color(palette::dim));
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

}  // namespace ion::ui
