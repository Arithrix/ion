#include "library/syx.h"

#include <algorithm>
#include <cstring>

namespace ion::syx {
namespace {

constexpr uint8_t kSysexStart = 0xF0;
constexpr uint8_t kSysexEnd = 0xF7;
constexpr uint8_t kYamaha = 0x43;

// Maximum value of each byte of an unpacked voice.
constexpr std::array<uint8_t, kVoiceSize> makeMaxima() {
  std::array<uint8_t, kVoiceSize> m{};
  // Six operators, 21 bytes each: R1-4, L1-4, break point, left depth, right depth,
  // left curve, right curve, rate scaling, amp mod sens, key vel sens, output level,
  // osc mode, coarse, fine, detune.
  constexpr uint8_t op[21] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                              3,  3,  7,  3,  7,  99, 1,  31, 99, 14};
  for (std::size_t o = 0; o < 6; ++o)
    for (std::size_t i = 0; i < 21; ++i) m[o * 21 + i] = op[i];
  // 126-133 pitch EG rates and levels, 134 algorithm, 135 feedback, 136 osc key sync,
  // 137 LFO speed, 138 delay, 139 PMD, 140 AMD, 141 LFO sync, 142 wave, 143 PMS, 144 transpose.
  constexpr uint8_t global[19] = {99, 99, 99, 99, 99, 99, 99, 99, 31, 7, 1, 99, 99, 99, 99, 1, 5, 7, 48};
  for (std::size_t i = 0; i < 19; ++i) m[126 + i] = global[i];
  for (std::size_t i = 0; i < kNameLength; ++i) m[kNameOffset + i] = 127;
  return m;
}
constexpr auto kMaxima = makeMaxima();

bool hasHeader(std::span<const uint8_t> b, std::size_t i, uint8_t format, uint8_t countHi, uint8_t countLo) {
  return i + 6 <= b.size() && b[i] == kSysexStart && b[i + 1] == kYamaha && (b[i + 2] & 0xF0) == 0x00 &&
         b[i + 3] == format && b[i + 4] == countHi && b[i + 5] == countLo;
}

}  // namespace

uint8_t checksum(std::span<const uint8_t> data) {
  unsigned sum = 0;
  for (uint8_t b : data) sum += b;
  return static_cast<uint8_t>((~sum + 1) & 0x7F);
}

ParseResult parse(std::span<const uint8_t> bytes) {
  ParseResult result;

  if (bytes.size() == kBulkDataSize && (bytes.empty() || bytes[0] != kSysexStart)) {
    result.dumps.push_back({DumpKind::Headerless, true, true, 0, {bytes.begin(), bytes.end()}});
    return result;
  }

  std::size_t i = 0;
  while (i < bytes.size()) {
    if (bytes[i] != kSysexStart) {
      ++i;
      continue;
    }
    std::size_t dataSize = 0;
    DumpKind kind{};
    if (hasHeader(bytes, i, 0x09, 0x20, 0x00)) {
      kind = DumpKind::Bulk;
      dataSize = kBulkDataSize;
    } else if (hasHeader(bytes, i, 0x00, 0x01, 0x1B)) {
      kind = DumpKind::Single;
      dataSize = kVoiceSize;
    } else {
      ++i;  // some other sysex message; skip to the next F0
      continue;
    }

    const std::size_t dataStart = i + 6;
    if (dataStart + dataSize > bytes.size()) {
      result.warnings.push_back("truncated " + std::string(kind == DumpKind::Bulk ? "bulk" : "single") +
                                " dump at byte " + std::to_string(i));
      break;
    }
    const auto data = bytes.subspan(dataStart, dataSize);
    const std::size_t csPos = dataStart + dataSize;
    const bool hasChecksum = csPos < bytes.size();
    const bool checksumOk = hasChecksum && bytes[csPos] == checksum(data);
    const bool terminated = csPos + 1 < bytes.size() && bytes[csPos + 1] == kSysexEnd;

    if (!checksumOk) result.warnings.push_back("checksum mismatch at byte " + std::to_string(i));
    if (!terminated) result.warnings.push_back("missing F7 at byte " + std::to_string(i));

    result.dumps.push_back({kind, checksumOk, terminated, i, {data.begin(), data.end()}});
    i = csPos + (terminated ? 2 : 1);
  }

  if (result.dumps.empty()) result.warnings.push_back("no DX7 voice data found");
  return result;
}

Voice normalize(const Voice& voice) {
  Voice out;
  for (std::size_t i = 0; i < kVoiceSize; ++i) out[i] = std::min(voice[i], kMaxima[i]);
  for (std::size_t i = 0; i < kNameLength; ++i) {
    uint8_t& c = out[kNameOffset + i];
    if (c < 32 || c > 126) c = ' ';
  }
  return out;
}

Voice unpack(std::span<const uint8_t, kPackedVoiceSize> p) {
  Voice v{};
  for (std::size_t op = 0; op < 6; ++op) {
    const uint8_t* in = p.data() + op * 17;
    uint8_t* out = v.data() + op * 21;
    for (std::size_t i = 0; i < 11; ++i) out[i] = in[i] & 0x7F;  // rates, levels, scaling
    out[11] = in[11] & 0x03;         // left curve
    out[12] = (in[11] >> 2) & 0x03;  // right curve
    out[13] = in[12] & 0x07;         // rate scaling
    out[14] = in[13] & 0x03;         // amp mod sensitivity
    out[15] = (in[13] >> 2) & 0x07;  // key velocity sensitivity
    out[16] = in[14] & 0x7F;         // output level
    out[17] = in[15] & 0x01;         // osc mode
    out[18] = (in[15] >> 1) & 0x1F;  // coarse
    out[19] = in[16] & 0x7F;         // fine
    out[20] = (in[12] >> 3) & 0x0F;  // detune
  }
  for (std::size_t i = 0; i < 8; ++i) v[126 + i] = p[102 + i] & 0x7F;  // pitch EG
  v[134] = p[110] & 0x1F;         // algorithm
  v[135] = p[111] & 0x07;         // feedback
  v[136] = (p[111] >> 3) & 0x01;  // osc key sync
  v[137] = p[112] & 0x7F;         // LFO speed
  v[138] = p[113] & 0x7F;         // LFO delay
  v[139] = p[114] & 0x7F;         // LFO pitch mod depth
  v[140] = p[115] & 0x7F;         // LFO amp mod depth
  v[141] = p[116] & 0x01;         // LFO sync
  v[142] = (p[116] >> 1) & 0x07;  // LFO wave
  // Pitch mod sensitivity is 3 bits (4-6). Synth_Dexed's own decodeVoice masks
  // only 2 (0x30); Dexed reads 3. We match Dexed.
  v[143] = (p[116] >> 4) & 0x07;
  v[144] = p[117] & 0x7F;  // transpose
  for (std::size_t i = 0; i < kNameLength; ++i) v[kNameOffset + i] = p[118 + i] & 0x7F;
  return normalize(v);
}

PackedVoice pack(const Voice& v) {
  PackedVoice p{};
  for (std::size_t op = 0; op < 6; ++op) {
    const uint8_t* in = v.data() + op * 21;
    uint8_t* out = p.data() + op * 17;
    std::memcpy(out, in, 11);
    out[11] = static_cast<uint8_t>((in[11] & 0x03) | ((in[12] & 0x03) << 2));
    out[12] = static_cast<uint8_t>((in[13] & 0x07) | ((in[20] & 0x0F) << 3));
    out[13] = static_cast<uint8_t>((in[14] & 0x03) | ((in[15] & 0x07) << 2));
    out[14] = in[16];
    out[15] = static_cast<uint8_t>((in[17] & 0x01) | ((in[18] & 0x1F) << 1));
    out[16] = in[19];
  }
  std::memcpy(p.data() + 102, v.data() + 126, 8);
  p[110] = v[134] & 0x1F;
  p[111] = static_cast<uint8_t>((v[135] & 0x07) | ((v[136] & 0x01) << 3));
  std::memcpy(p.data() + 112, v.data() + 137, 4);
  p[116] = static_cast<uint8_t>((v[141] & 0x01) | ((v[142] & 0x07) << 1) | ((v[143] & 0x07) << 4));
  p[117] = v[144];
  std::memcpy(p.data() + 118, v.data() + kNameOffset, kNameLength);
  return p;
}

std::vector<Voice> voices(const Dump& dump) {
  std::vector<Voice> out;
  if (dump.kind == DumpKind::Single) {
    Voice v;
    std::copy_n(dump.data.begin(), kVoiceSize, v.begin());
    out.push_back(normalize(v));
    return out;
  }
  out.reserve(32);
  for (std::size_t i = 0; i < 32; ++i)
    out.push_back(unpack(std::span<const uint8_t, kPackedVoiceSize>(dump.data.data() + i * kPackedVoiceSize,
                                                                     kPackedVoiceSize)));
  return out;
}

std::string name(const Voice& voice) {
  std::string s;
  for (std::size_t i = 0; i < kNameLength; ++i) {
    const auto c = static_cast<char>(voice[kNameOffset + i]);
    s += (c >= 32 && c <= 126) ? c : ' ';
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

std::vector<uint8_t> makeBulkDump(std::span<const PackedVoice, 32> voices) {
  std::vector<uint8_t> out = {kSysexStart, kYamaha, 0x00, 0x09, 0x20, 0x00};
  out.reserve(kBulkDumpSize);
  for (const auto& v : voices) out.insert(out.end(), v.begin(), v.end());
  out.push_back(checksum(std::span<const uint8_t>(out.data() + 6, kBulkDataSize)));
  out.push_back(kSysexEnd);
  return out;
}

std::vector<uint8_t> makeSingleDump(const Voice& voice) {
  std::vector<uint8_t> out = {kSysexStart, kYamaha, 0x00, 0x00, 0x01, 0x1B};
  out.insert(out.end(), voice.begin(), voice.end());
  out.push_back(checksum(voice));
  out.push_back(kSysexEnd);
  return out;
}

}  // namespace ion::syx
