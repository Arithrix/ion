#include "midi/smf.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ion::smf {
namespace {

struct Reader {
  std::span<const uint8_t> b;
  std::size_t pos = 0;
  bool ok = true;

  uint8_t u8() {
    if (pos >= b.size()) {
      ok = false;
      return 0;
    }
    return b[pos++];
  }
  uint32_t be(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | u8();
    return v;
  }
  uint32_t vlq() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t c = u8();
      v = v << 7 | (c & 0x7F);
      if (!(c & 0x80)) return v;
    }
    ok = false;  // longer than 4 bytes: corrupt
    return v;
  }
};

struct TickEvent {
  uint64_t tick;
  int order;  // file order, so simultaneous events keep their sequence
  MidiMessage msg;
};

}  // namespace

bool parse(std::span<const uint8_t> bytes, File& out, std::string& error) {
  Reader r{bytes};
  if (bytes.size() < 14 || r.be(4) != 0x4D546864 /* MThd */) {
    error = "not a MIDI file";
    return false;
  }
  const uint32_t headerLen = r.be(4);
  const uint32_t format = r.be(2);
  const uint32_t trackCount = r.be(2);
  const uint32_t division = r.be(2);
  r.pos = 8 + headerLen;
  if (format > 1) {
    error = "MIDI file format " + std::to_string(format) + " isn't supported (only 0 and 1)";
    return false;
  }
  if (division & 0x8000) {
    error = "SMPTE time division isn't supported";
    return false;
  }
  const double ppq = division ? division : 480;

  std::vector<TickEvent> events;
  std::map<uint64_t, uint32_t> tempo;  // tick → µs per quarter
  uint64_t lastTick = 0;
  int order = 0;

  for (uint32_t t = 0; t < trackCount && r.ok && r.pos + 8 <= bytes.size(); ++t) {
    const uint32_t id = r.be(4);
    const uint32_t len = r.be(4);
    const std::size_t end = std::min(bytes.size(), r.pos + len);
    if (id != 0x4D54726B /* MTrk */) {
      r.pos = end;  // unknown chunk: skip
      continue;
    }
    uint64_t tick = 0;
    uint8_t running = 0;
    while (r.ok && r.pos < end) {
      tick += r.vlq();
      uint8_t status = r.u8();
      if (status < 0x80) {  // running status
        if (!running) {
          r.ok = false;
          break;
        }
        --r.pos;
        status = running;
      }
      if (status == 0xFF) {
        const uint8_t type = r.u8();
        const uint32_t n = r.vlq();
        if (type == 0x51 && n == 3) tempo[tick] = r.be(3);
        else r.pos += n;
        if (type == 0x2F) break;  // end of track
      } else if (status == 0xF0 || status == 0xF7) {
        r.pos += r.vlq();  // sysex: skipped
      } else {
        running = status;
        const uint8_t type = status & 0xF0;
        const uint8_t d1 = r.u8();
        const uint8_t d2 = (type == 0xC0 || type == 0xD0) ? 0 : r.u8();
        events.push_back({tick, order++, {status, d1, d2}});
      }
      lastTick = std::max(lastTick, tick);
    }
    r.pos = end;
  }
  if (!r.ok) {
    error = "MIDI file is truncated or corrupt";
    return false;
  }

  std::stable_sort(events.begin(), events.end(), [](const TickEvent& a, const TickEvent& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
  });
  if (!tempo.count(0)) tempo[0] = 500000;  // 120 BPM

  // Tick → seconds through the tempo map.
  auto seconds = [&](uint64_t tick) {
    double s = 0;
    uint64_t prevTick = 0;
    uint32_t us = tempo.begin()->second;
    for (const auto& [at, value] : tempo) {
      if (at >= tick) break;
      s += static_cast<double>(at - prevTick) * us / ppq / 1e6;
      prevTick = at;
      us = value;
    }
    return s + static_cast<double>(tick - prevTick) * us / ppq / 1e6;
  };

  out.events.clear();
  for (const TickEvent& e : events) out.events.push_back({seconds(e.tick), e.msg});
  out.lengthSeconds = seconds(lastTick);
  return true;
}

std::vector<uint8_t> write(const std::vector<Event>& events, double bpm) {
  constexpr uint32_t ppq = 480;
  auto be = [](std::vector<uint8_t>& v, uint32_t x, int n) {
    for (int i = n - 1; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
  };
  auto vlq = [](std::vector<uint8_t>& v, uint32_t x) {
    uint8_t buf[4];
    int n = 0;
    do {
      buf[n++] = x & 0x7F;
      x >>= 7;
    } while (x && n < 4);
    while (n--) v.push_back(static_cast<uint8_t>(buf[n] | (n ? 0x80 : 0)));
  };

  std::vector<Event> sorted = events;
  std::stable_sort(sorted.begin(), sorted.end(), [](const Event& a, const Event& b) { return a.seconds < b.seconds; });

  std::vector<uint8_t> track;
  const uint32_t usPerQuarter = static_cast<uint32_t>(std::lround(60e6 / bpm));
  track.insert(track.end(), {0x00, 0xFF, 0x51, 0x03});
  be(track, usPerQuarter, 3);
  uint64_t lastTick = 0;
  for (const Event& e : sorted) {
    const auto tick = static_cast<uint64_t>(std::llround(e.seconds * bpm / 60.0 * ppq));
    vlq(track, static_cast<uint32_t>(tick - lastTick));
    lastTick = tick;
    track.push_back(e.msg.status);
    track.push_back(e.msg.data1);
    const uint8_t type = e.msg.status & 0xF0;
    if (type != 0xC0 && type != 0xD0) track.push_back(e.msg.data2);
  }
  track.insert(track.end(), {0x00, 0xFF, 0x2F, 0x00});

  std::vector<uint8_t> file = {'M', 'T', 'h', 'd'};
  be(file, 6, 4);
  be(file, 0, 2);
  be(file, 1, 2);
  be(file, ppq, 2);
  file.insert(file.end(), {'M', 'T', 'r', 'k'});
  be(file, static_cast<uint32_t>(track.size()), 4);
  file.insert(file.end(), track.begin(), track.end());
  return file;
}

}  // namespace ion::smf
