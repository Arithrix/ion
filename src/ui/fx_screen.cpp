// Effects (F): the selected track's signal chain, the send buses and the
// master (mockups/FX; HANDOFF §7). Left, the chain: EQ, three insert slots and
// the sends, then the two returns, then the master's EQ, slots and limiter.
// Right, the selected effect's settings.

#include <cmath>
#include <cstdio>

#include "engine/fx/effects.h"
#include "engine/module.h"
#include "project/session.h"
#include "ui/screens.h"

namespace ion::ui {
namespace {

using namespace ftxui;

struct Item {
  enum class Kind { Eq, Slot, Send, SendPre, Bus, Limiter } kind;
  int scope = 0;
  int slot = 0;  // Slot; Send: 0 = A, 1 = B
};

int roundDb(float db) { return static_cast<int>(std::lround(db)); }

std::string dbShort(float db) {
  if (db <= kFaderMinDb) return "−∞";
  const int n = roundDb(db);
  return n < 0 ? "−" + std::to_string(-n) : (n > 0 ? "+" : "") + std::to_string(n);
}

std::string paramText(const Session& s, ParamRef ref) {
  const ParamInfo* pi = s.paramInfo(ref);
  return pi ? formatValue(*pi, s.param(ref)) : "?";
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

class FxScreen final : public Screen {
 public:
  Element render(Context& ctx) override {
    const auto list = items(ctx);
    sel_ = std::clamp(sel_, 0, static_cast<int>(list.size()) - 1);
    if (ctx.narrow()) return (pane_ == 0 ? chain(ctx, list) : detail(ctx, list[static_cast<std::size_t>(sel_)])) | flex;
    const int left = std::clamp(ctx.width * 42 / 100, 36, 60);
    return hbox({chain(ctx, list) | size(WIDTH, EQUAL, left), detail(ctx, list[static_cast<std::size_t>(sel_)]) | flex});
  }

  bool onEvent(Context& ctx, const Event& e) override {
    Session& s = ctx.session;
    const auto list = items(ctx);
    sel_ = std::clamp(sel_, 0, static_cast<int>(list.size()) - 1);
    const Item it = list[static_cast<std::size_t>(sel_)];

    if (picking_) {
      if (e == Event::ArrowUp) { pick_ = (pick_ + kEffectTypes - 2) % (kEffectTypes - 1); return true; }
      if (e == Event::ArrowDown) { pick_ = (pick_ + 1) % (kEffectTypes - 1); return true; }
      if (e == Event::Escape) { picking_ = false; return true; }
      if (e == Event::Return) {
        const auto type = static_cast<EffectType>(pick_ + 1);
        s.setInsert(it.scope, it.slot, type);
        picking_ = false;
        pane_ = 1;
        row_ = 0;
        ctx.flash = std::string(effectLabel(type)) + " in " + scopeLabel(it.scope) + " slot " + std::to_string(it.slot + 1);
        return true;
      }
      return true;  // the picker has the keyboard
    }

    if (e == Event::Tab || e == Event::TabReverse) {
      pane_ = 1 - pane_;
      return true;
    }
    if (e == Event::Character('a') && it.kind == Item::Kind::Slot && !s.insertFixed(it.scope, it.slot)) {
      openPicker(s, it);
      return true;
    }
    if (e == Event::Character('x') && it.kind == Item::Kind::Slot && !s.insertFixed(it.scope, it.slot) &&
        s.insert(it.scope, it.slot) != EffectType::None) {
      s.setInsert(it.scope, it.slot, EffectType::None);
      ctx.flash = scopeLabel(it.scope) + " slot " + std::to_string(it.slot + 1) + " emptied";
      return true;
    }
    if (e == Event::Character('b')) {
      if (it.kind == Item::Kind::Eq) s.nudgeParam({static_cast<uint8_t>(it.scope), kEqBypass}, 1, false);
      else if (it.kind == Item::Kind::Slot && s.insert(it.scope, it.slot) != EffectType::None && !s.insertFixed(it.scope, it.slot))
        s.nudgeParam(insertOnRef(it.scope, it.slot), 1, false);
      return true;
    }
    if ((e == Event::Character('[') || e == Event::Character(']')) && it.kind == Item::Kind::Slot) {
      const int d = e == Event::Character('[') ? -1 : 1;
      const int to = it.slot + d;
      if (to >= 0 && to < kInsertSlots && !s.insertFixed(it.scope, to)) {
        s.moveInsert(it.scope, it.slot, d);
        sel_ += d;
      }
      return true;
    }

    if (pane_ == 1) return detailEvent(ctx, it, e);

    const int n = static_cast<int>(list.size());
    if (e == Event::ArrowUp) { sel_ = (sel_ + n - 1) % n; row_ = 0; return true; }
    if (e == Event::ArrowDown) { sel_ = (sel_ + 1) % n; row_ = 0; return true; }
    if (const Nudge nd = nudgeOf(e); nd.direction) {
      if (it.kind == Item::Kind::Send) s.nudgeParam({static_cast<uint8_t>(it.scope), it.slot == 0 ? kSendA : kSendB}, nd.direction, nd.coarse);
      if (it.kind == Item::Kind::SendPre) s.nudgeParam({static_cast<uint8_t>(it.scope), kSendPre}, 1, false);
      return true;
    }
    if (e == Event::Return) {
      if (it.kind == Item::Kind::Eq) {
        if (it.scope < kTrackCount) ctx.track = it.scope;
        ctx.request = Context::Request::ShowMixer;
      } else if (it.kind == Item::Kind::Slot && s.insert(it.scope, it.slot) == EffectType::None) {
        openPicker(s, it);
      } else if (it.kind == Item::Kind::Slot || it.kind == Item::Kind::Bus) {
        pane_ = 1;
      } else if (it.kind == Item::Kind::SendPre) {
        s.nudgeParam({static_cast<uint8_t>(it.scope), kSendPre}, 1, false);
      }
      return true;
    }
    return false;
  }

  Hints hints(Context& ctx) override {
    if (picking_) return {{"↑↓", "Effect"}, {"⏎", "Add"}, {"Esc", "Cancel"}};
    if (pane_ == 1) return {{"↑↓", "Param"}, {"←→", "Adjust"}, {"l", "Learn"}, {"b", "Bypass"}, {"Tab", "Chain"}, {"Esc", "Back"}};
    Hints h = {{"↑↓", "Slot"}, {"⏎", "Edit"}, {"a", "Add"}, {"x", "Remove"}, {"b", "Bypass"}};
    if (!ctx.narrow()) h.push_back({"[ ]", "Reorder"});
    h.push_back({"←→", "Send"});
    h.push_back({"Tab", "Params"});
    h.push_back({"Esc", "Back"});
    return h;
  }

  std::optional<ParamRef> focusedParam(Context& ctx) override {
    const auto list = items(ctx);
    const Item it = list[static_cast<std::size_t>(std::clamp(sel_, 0, static_cast<int>(list.size()) - 1))];
    if (pane_ == 0) {
      if (it.kind == Item::Kind::Send) return ParamRef{static_cast<uint8_t>(it.scope), it.slot == 0 ? kSendA : kSendB};
      return std::nullopt;
    }
    const auto rows = paramRows(ctx.session, it);
    if (rows.empty()) return std::nullopt;
    return rows[static_cast<std::size_t>(std::clamp(row_, 0, static_cast<int>(rows.size()) - 1))];
  }

 private:
  void openPicker(const Session& s, const Item& it) {
    picking_ = true;
    pane_ = 1;
    const EffectType cur = s.insert(it.scope, it.slot);
    pick_ = cur == EffectType::None ? 0 : static_cast<int>(cur) - 1;
  }

  std::vector<Item> items(const Context& ctx) const {
    std::vector<Item> out;
    const int t = ctx.track;
    out.push_back({Item::Kind::Eq, t});
    for (int i = 0; i < kInsertSlots; ++i) out.push_back({Item::Kind::Slot, t, i});
    out.push_back({Item::Kind::Send, t, 0});
    out.push_back({Item::Kind::Send, t, 1});
    out.push_back({Item::Kind::SendPre, t});
    out.push_back({Item::Kind::Bus, kDelayScope, 0});
    out.push_back({Item::Kind::Bus, kReverbScope, 0});
    out.push_back({Item::Kind::Eq, kMasterScope});
    for (int i = 0; i < kInsertSlots; ++i) out.push_back({Item::Kind::Slot, kMasterScope, i});
    out.push_back({Item::Kind::Limiter, kMasterScope});
    return out;
  }

  // The parameters the detail pane edits, in order.
  std::vector<ParamRef> paramRows(const Session& s, const Item& it) const {
    std::vector<ParamRef> rows;
    if (it.kind != Item::Kind::Slot && it.kind != Item::Kind::Bus) return rows;
    const EffectType type = s.insert(it.scope, it.slot);
    if (type == EffectType::None) return rows;
    const bool bus = s.insertFixed(it.scope, it.slot);
    if (!bus) rows.push_back(insertOnRef(it.scope, it.slot));
    const auto table = effectParams(type);
    for (std::size_t i = 0; i < table.size(); ++i) {
      if (bus && std::string(table[i].key) == "mix") continue;  // a return is all wet
      rows.push_back(insertParamRef(it.scope, it.slot, static_cast<int>(i)));
    }
    return rows;
  }

  bool detailEvent(Context& ctx, const Item& it, const Event& e) {
    Session& s = ctx.session;
    const auto rows = paramRows(s, it);
    if (rows.empty()) return false;
    const int n = static_cast<int>(rows.size());
    if (e == Event::ArrowUp) { row_ = (row_ + n - 1) % n; return true; }
    if (e == Event::ArrowDown) { row_ = (row_ + 1) % n; return true; }
    row_ = std::clamp(row_, 0, n - 1);
    if (const Nudge nd = nudgeOf(e); nd.direction) {
      s.nudgeParam(rows[static_cast<std::size_t>(row_)], nd.direction, nd.coarse);
      return true;
    }
    return false;
  }

  // ---- the chain ----

  Element chain(Context& ctx, const std::vector<Item>& list) {
    const Session& s = ctx.session;
    const ModuleStatus& st = ctx.module.status();
    const bool focused = pane_ == 0 && !picking_;
    const int inner = std::max(20, (ctx.narrow() ? ctx.width : std::clamp(ctx.width * 42 / 100, 36, 60)) - 4);
    Elements rows;
    auto heading = [&](const std::string& t) {
      if (!rows.empty()) rows.push_back(text(""));
      rows.push_back(text(fit(t, inner)) | color(palette::dim));
    };
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
      const Item& it = list[static_cast<std::size_t>(i)];
      if (i == 0) heading("track " + std::to_string(ctx.track + 1) + " · " + trackSoundName(s, ctx.track));
      if (it.kind == Item::Kind::Bus && it.scope == kDelayScope) heading("send buses");
      if (it.kind == Item::Kind::Eq && it.scope == kMasterScope) heading("master");

      std::string slot, name, summary, on;
      Color onColor = palette::faint;
      const auto sc = static_cast<uint8_t>(it.scope);
      switch (it.kind) {
        case Item::Kind::Eq: {
          name = "EQ";
          const bool bypassed = s.param({sc, kEqBypass}) >= 0.5f;
          summary = eqSummary(s, it.scope);
          on = bypassed ? "○" : "●";
          onColor = bypassed ? palette::faint : palette::value;
          break;
        }
        case Item::Kind::Slot: {
          slot = std::to_string(it.slot + 1);
          const EffectType type = s.insert(it.scope, it.slot);
          name = effectLabel(type);
          summary = insertSummary(s, it.scope, it.slot);
          if (type != EffectType::None) {
            const bool in = s.param(insertOnRef(it.scope, it.slot)) >= 0.5f;
            on = in ? "●" : "○";
            onColor = in ? palette::value : palette::faint;
            const float gr = st.reduction[it.scope][static_cast<std::size_t>(it.slot)].load(std::memory_order_relaxed);
            if (type == EffectType::Compressor && gr > 0.05f) summary += " · GR " + dbShort(-gr);
          }
          break;
        }
        case Item::Kind::Send:
          slot = it.slot == 0 ? "A" : "B";
          name = it.slot == 0 ? "→ delay" : "→ reverb";
          summary = paramText(s, {sc, it.slot == 0 ? kSendA : kSendB});
          if (pane_ == 0 && i == sel_) summary = "◀ " + summary + " ▶";
          break;
        case Item::Kind::SendPre:
          name = "sends";
          summary = s.param({sc, kSendPre}) >= 0.5f ? "pre-fader" : "post-fader";
          break;
        case Item::Kind::Bus:
          slot = it.scope == kDelayScope ? "A" : "B";
          name = effectLabel(s.insert(it.scope, 0));
          summary = insertSummary(s, it.scope, 0) + " · " + dbShort(s.param({sc, kFader})) + " dB";
          on = s.param({sc, kMute}) >= 0.5f ? "M" : "●";
          onColor = s.param({sc, kMute}) >= 0.5f ? palette::record : palette::value;
          break;
        case Item::Kind::Limiter: {
          name = "LIMIT";
          const float gr = st.limiter.load(std::memory_order_relaxed);
          summary = "ceiling −0.3 dB" + (gr > 0.05f ? " · GR " + dbShort(-gr) : std::string());
          on = "●";
          onColor = palette::value;
          break;
        }
      }
      const bool selected = i == sel_;
      const bool empty = name == "—";
      Color fg = selected ? palette::bg : (empty ? palette::faint : palette::text);
      const int nameW = inner >= 44 ? 12 : 9;
      Element row = hbox({text(" " + slot) | size(WIDTH, EQUAL, 3) | color(selected ? palette::bg : palette::dim),
                          text(name) | size(WIDTH, EQUAL, nameW) | color(fg),
                          text(fit(summary, inner - nameW - 6)) | color(selected ? palette::bg : (empty ? palette::faint : palette::dim)), filler(),
                          text(on + " ") | color(selected ? palette::bg : onColor)});
      if (selected) row = row | bgcolor(focused ? palette::accent : palette::raised);
      Hit h;
      h.focus = [this, i](Context&) {
        if (picking_) return;
        pane_ = 0;
        if (sel_ != i) row_ = 0;
        sel_ = i;
      };
      h.pane = [this](Context&) { if (!picking_) pane_ = 0; };
      h.drag = Hit::Drag::Horizontal;  // a send's level
      rows.push_back(ctx.hit(row, std::move(h)));
    }
    rows.push_back(filler());
    return pane(ctx.narrow() ? "Chain · Tab ⇄" : "Signal chain", vbox(rows),
                ctx.piano ? palette::value : (focused ? palette::accent : palette::border));
  }

  // ---- the detail pane ----

  Element paramList(Context& ctx, const Item& it, int width) {
    const Session& s = ctx.session;
    const auto rows = paramRows(s, it);
    Elements out;
    const int labelW = 11, valueW = 10;
    const int barW = std::clamp(width - 4 - labelW - valueW - 3, 0, 24);
    row_ = std::clamp(row_, 0, std::max(0, static_cast<int>(rows.size()) - 1));
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
      const ParamRef ref = rows[static_cast<std::size_t>(r)];
      const ParamInfo* pi = s.paramInfo(ref);
      if (!pi) continue;
      const float v = s.param(ref);
      const bool on = pane_ == 1 && r == row_;
      Element bar = text(std::string(static_cast<std::size_t>(barW), ' '));
      if (barW >= 4 && pi->curve != Curve::Toggle && pi->curve != Curve::Choice) {
        bar = std::string(pi->key) == "pan" ? centredBar(v, barW, on) : valueBar(toNormalized(*pi, v), barW, palette::value, false, modulatedAt(ctx, ref));
      }
      Element line = hbox({text(on ? "▸ " : "  ") | color(palette::accent),
                           text(pi->label) | size(WIDTH, EQUAL, labelW) | color(on ? palette::accent : palette::text), text(" "),
                           bar | size(WIDTH, EQUAL, barW), text(" "), text(formatValue(*pi, v)) | color(on ? palette::text : palette::dim),
                           filler()});
      out.push_back(ctx.value(on ? line | bgcolor(palette::raised) : line, [this, r](Context&) {
        if (picking_) return;
        pane_ = 1;
        row_ = r;
      }, std::nullopt));
    }
    return vbox(out);
  }

  // The reverb's tail as it'll sound: pre-delay, a short build, the decay
  // over 60 dB. Three rows of block characters.
  Element reverbTail(const Session& s, int scope, int slot, int width) {
    static const char* levels[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    auto p = [&](int i) { return s.param(insertParamRef(scope, slot, i)); };
    const float decay = p(Reverb::kDecay) / 1000.0f, pre = p(Reverb::kPreDelay) / 1000.0f, size = p(Reverb::kSize);
    const float span = pre + decay * 1.1f;
    const float build = 0.01f + 0.04f * size;
    Elements rowsOut;
    for (int row = 2; row >= 0; --row) {
      std::string line;
      for (int x = 0; x < width; ++x) {
        const float t = span * static_cast<float>(x) / static_cast<float>(width);
        float db = -100;
        if (t >= pre) {
          const float since = t - pre;
          const float rise = std::min(1.0f, since / build);
          db = -60.0f * since / decay + 20.0f * std::log10(std::max(rise, 1e-3f));
        }
        const float level = std::clamp((db + 60.0f) / 60.0f * 24.0f, 0.0f, 24.0f);
        const int fill = std::clamp(static_cast<int>(std::lround(level)) - row * 8, 0, 8);
        line += levels[fill];
      }
      rowsOut.push_back(text(line) | color(palette::value));
    }
    char axis[64];
    std::snprintf(axis, sizeof axis, "0 … %.1f s · −60 dB at %.1f s", static_cast<double>(span), static_cast<double>(pre + decay));
    rowsOut.push_back(text(axis) | color(palette::faint));
    return vbox(rowsOut);
  }

  Element picker(Context& ctx, const Item& it) {
    Elements rows = {text("put an effect in " + scopeLabel(it.scope) + " slot " + std::to_string(it.slot + 1)) | color(palette::dim),
                     text("")};
    static const char* about[] = {"", "level control: threshold, ratio, knee, attack, release", "width and movement: two modulated voices",
                                  "echoes, synced to the clock or in ms", "plate, hall or room",
                                  "distortion: soft, hard or folding, with tone"};
    for (int i = 0; i < kEffectTypes - 1; ++i) {
      const auto type = static_cast<EffectType>(i + 1);
      const bool on = i == pick_;
      Element r = hbox({text(on ? " ▸ " : "   "), text(effectLabel(type)) | size(WIDTH, EQUAL, 9), text(about[i + 1]) | color(on ? palette::bg : palette::dim)});
      rows.push_back(ctx.item(on ? r | color(palette::bg) | bgcolor(palette::accent) : r, [this, i](Context&) { pick_ = i; }));
    }
    rows.push_back(text(""));
    rows.push_back(text("⏎ adds it · Esc cancels") | color(palette::faint));
    rows.push_back(filler());
    return pane("Add effect", vbox(rows), palette::accent);
  }

  Element detail(Context& ctx, const Item& it) {
    const Session& s = ctx.session;
    const ModuleStatus& st = ctx.module.status();
    if (picking_) return picker(ctx, it);
    const int width = ctx.narrow() ? ctx.width : ctx.width - std::clamp(ctx.width * 42 / 100, 36, 60);
    const bool focused = pane_ == 1;
    const Color border = ctx.piano ? palette::value : (focused ? palette::accent : palette::border);
    Elements out;
    std::string title;
    switch (it.kind) {
      case Item::Kind::Eq:
        title = "EQ · " + scopeLabel(it.scope);
        out.push_back(text(eqSummary(s, it.scope)) | color(palette::text));
        out.push_back(text(""));
        out.push_back(paragraph("The channel EQ sits before the inserts. ⏎ opens it in the Mixer; b bypasses it.") | color(palette::dim));
        break;
      case Item::Kind::Send:
      case Item::Kind::SendPre:
        title = "Sends · " + scopeLabel(it.scope);
        out.push_back(hbox({text("A → delay   "), text(paramText(s, {static_cast<uint8_t>(it.scope), kSendA})) | color(palette::value)}));
        out.push_back(hbox({text("B → reverb  "), text(paramText(s, {static_cast<uint8_t>(it.scope), kSendB})) | color(palette::value)}));
        out.push_back(text(std::string("taken ") + (s.param({static_cast<uint8_t>(it.scope), kSendPre}) >= 0.5f
                                                        ? "before the fader: the fader doesn't change them"
                                                        : "after the fader: they follow it")) |
                      color(palette::dim));
        out.push_back(text(""));
        out.push_back(paragraph("←→ on a send sets its level (Shift for bigger steps); l learns a CC. Mute silences the sends too.") |
                      color(palette::faint));
        break;
      case Item::Kind::Limiter: {
        title = "Limiter · master";
        const float gr = st.limiter.load(std::memory_order_relaxed);
        out.push_back(text("ceiling −0.3 dBFS · 1 ms lookahead") | color(palette::text));
        out.push_back(hbox({text("reduction "), levelBar(gr > 0 ? std::pow(10.0f, (gr - 48.0f) / 20.0f) : 0, 20),
                            text(" " + (gr > 0.05f ? dbShort(-gr) + " dB" : std::string("none"))) | color(gr > 0.05f ? palette::accent : palette::dim)}));
        out.push_back(text(""));
        out.push_back(paragraph("Always on and last: nothing reaches the speakers above −0.3 dBFS. If it's working often, "
                                "turn the master or the loudest track down.") |
                      color(palette::dim));
        break;
      }
      case Item::Kind::Slot:
      case Item::Kind::Bus: {
        const EffectType type = s.insert(it.scope, it.slot);
        title = std::string(effectLabel(type)) + " · " +
                (it.kind == Item::Kind::Bus ? std::string(it.scope == kDelayScope ? "send A" : "send B")
                                            : scopeLabel(it.scope) + " slot " + std::to_string(it.slot + 1));
        if (type == EffectType::None) {
          out.push_back(text("empty") | color(palette::faint));
          out.push_back(text(""));
          out.push_back(text("⏎ or a adds an effect") | color(palette::dim));
          break;
        }
        if (type == EffectType::Reverb) {
          const int kind = static_cast<int>(std::lround(s.param(insertParamRef(it.scope, it.slot, Reverb::kType))));
          Elements kinds;
          static const char* names[] = {"Plate", "Hall", "Room"};
          for (int k = 0; k < 3; ++k)
            kinds.push_back(text(" " + std::string(names[k]) + " ") | (k == kind ? color(palette::bg) | bgcolor(palette::accent) : color(palette::dim)));
          out.push_back(hbox(kinds));
          out.push_back(text(""));
        }
        out.push_back(paramList(ctx, it, width));
        out.push_back(text(""));
        if (type == EffectType::Reverb) out.push_back(reverbTail(s, it.scope, it.slot, std::max(10, width - 6)));
        if (type == EffectType::Compressor) {
          const float gr = st.reduction[it.scope][static_cast<std::size_t>(it.slot)].load(std::memory_order_relaxed);
          out.push_back(hbox({text("reduction "), levelBar(gr > 0 ? std::pow(10.0f, (gr - 48.0f) / 20.0f) : 0, 20),
                              text(" " + (gr > 0.05f ? dbShort(-gr) + " dB" : std::string("none"))) | color(gr > 0.05f ? palette::accent : palette::dim)}));
        }
        if (type == EffectType::Delay) {
          const bool sync = s.param(insertParamRef(it.scope, it.slot, Delay::kSync)) >= 0.5f;
          const bool clock = st.clockPresent.load(std::memory_order_relaxed);
          const double bpm = clock ? st.bpm.load(std::memory_order_relaxed) : 120.0;
          char line[96];
          if (sync) {
            const double ms = Delay::divisionBeats(static_cast<int>(std::lround(s.param(insertParamRef(it.scope, it.slot, Delay::kDivision))))) *
                              60000.0 / bpm;
            std::snprintf(line, sizeof line, "%s = %.0f ms at %.1f BPM%s · Time is used when Sync is off",
                          paramText(s, insertParamRef(it.scope, it.slot, Delay::kDivision)).c_str(), ms, bpm, clock ? "" : " (no clock)");
          } else {
            std::snprintf(line, sizeof line, "Sync off: Division is ignored");
          }
          out.push_back(text(fit(line, width - 4)) | color(palette::dim));
        }
        if (it.kind == Item::Kind::Bus) {
          out.push_back(text(""));
          out.push_back(text(fit("return: " + dbShort(s.param({static_cast<uint8_t>(it.scope), kFader})) +
                                     " dB · fader, pan and EQ in the Mixer" +
                                     (it.scope == kDelayScope ? " · its send B feeds the reverb" : ""),
                                 width - 4)) |
                        color(palette::faint));
        }
        // CCs mapped to this effect.
        std::string ccs;
        for (const Session::CcEntry& c : s.ccMappings()) {
          const ParamRef& tgt = c.mapping.target;
          if (tgt.scope != it.scope || !tgt.isInsert() || tgt.slot() != it.slot) continue;
          const ParamInfo* pi = s.paramInfo(tgt);
          std::string label = pi ? pi->label : "?";
          for (char& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          ccs += (ccs.empty() ? "" : " · ") + std::string("cc ") + std::to_string(c.mapping.cc) + " → " + label;
        }
        if (!ccs.empty()) out.push_back(text(fit(ccs, width - 4)) | color(palette::value));
        break;
      }
    }
    out.push_back(filler());
    if (ctx.narrow()) title += " · Tab ⇄";
    return pane(fit(title, width - 6), vbox(out), border);
  }

  int sel_ = 1;   // slot 1 of the track
  int pane_ = 0;  // 0 chain, 1 parameters
  int row_ = 0;
  bool picking_ = false;
  int pick_ = 0;
};

}  // namespace

std::string insertSummary(const Session& s, int scope, int slot) {
  const EffectType type = s.insert(scope, slot);
  auto p = [&](int i) { return s.param(insertParamRef(scope, slot, i)); };
  auto txt = [&](int i) { return paramText(s, insertParamRef(scope, slot, i)); };
  char buf[64];
  switch (type) {
    case EffectType::None: return "empty";
    case EffectType::Compressor:
      std::snprintf(buf, sizeof buf, "thr %s · %s", dbShort(p(Compressor::kThreshold)).c_str(), txt(Compressor::kRatio).c_str());
      return buf;
    case EffectType::Chorus:
      std::snprintf(buf, sizeof buf, "rate %.2f · %s", static_cast<double>(p(Chorus::kRate)), txt(Chorus::kMix).c_str());
      return buf;
    case EffectType::Delay:
      std::snprintf(buf, sizeof buf, "%s · fb %s", (p(Delay::kSync) >= 0.5f ? txt(Delay::kDivision) : txt(Delay::kTime)).c_str(),
                    txt(Delay::kFeedback).c_str());
      return buf;
    case EffectType::Reverb:
      std::snprintf(buf, sizeof buf, "%s · %.1f s", txt(Reverb::kType).c_str(), static_cast<double>(p(Reverb::kDecay) / 1000.0f));
      return buf;
    case EffectType::Drive:
      std::snprintf(buf, sizeof buf, "%s · +%.0f dB", txt(Drive::kShape).c_str(), static_cast<double>(p(Drive::kDrive)));
      return buf;
  }
  return "";
}

std::unique_ptr<Screen> makeFxScreen() { return std::make_unique<FxScreen>(); }

}  // namespace ion::ui
