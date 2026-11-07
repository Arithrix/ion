#pragma once

// DX7 system-exclusive voice data: parsing, packing, checksums.
//
// Formats (Yamaha DX7 sysex spec):
//   32-voice bulk dump:  F0 43 0n 09 20 00 <4096 packed bytes> <checksum> F7   (4104 bytes)
//   single-voice dump:   F0 43 0n 00 01 1B <155 unpacked bytes> <checksum> F7  (163 bytes)
// A packed voice is 128 bytes; an unpacked voice is 155 (the layout Dexed's engine uses).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ion::syx {

inline constexpr std::size_t kPackedVoiceSize = 128;
inline constexpr std::size_t kVoiceSize = 155;
inline constexpr std::size_t kBulkDataSize = 32 * kPackedVoiceSize;  // 4096
inline constexpr std::size_t kBulkDumpSize = 6 + kBulkDataSize + 2;  // 4104
inline constexpr std::size_t kSingleDumpSize = 6 + kVoiceSize + 2;   // 163
inline constexpr std::size_t kNameOffset = 145;                      // in an unpacked voice
inline constexpr std::size_t kNameLength = 10;

using PackedVoice = std::array<uint8_t, kPackedVoiceSize>;
using Voice = std::array<uint8_t, kVoiceSize>;

enum class DumpKind { Bulk, Single, Headerless };

struct Dump {
  DumpKind kind;
  bool checksum_ok;          // Headerless dumps have no checksum and report true
  bool terminated;           // false when the trailing F7 was missing
  std::size_t offset;        // byte offset in the file
  std::vector<uint8_t> data; // 4096 packed bytes (Bulk, Headerless) or 155 unpacked (Single)
};

struct ParseResult {
  std::vector<Dump> dumps;
  std::vector<std::string> warnings;
};

// Finds every DX7 voice dump in a file's bytes. Tolerates other sysex messages
// before or after, a missing F7, and a bare 4096-byte file with no header.
// Never throws on bad input; problems go into `warnings`.
ParseResult parse(std::span<const uint8_t> bytes);

// Yamaha checksum: two's complement of the 7-bit sum of the data bytes.
uint8_t checksum(std::span<const uint8_t> data);

// Unpacks a 128-byte voice into the 155-byte layout. Bits the spec marks as
// "don't care" are masked off and out-of-range values are clamped, so the result
// is always safe to hand to the engine.
Voice unpack(std::span<const uint8_t, kPackedVoiceSize> packed);

// Inverse of unpack. pack(unpack(p)) == p for any spec-clean packed voice.
PackedVoice pack(const Voice& voice);

// Clamps every parameter of an unpacked voice into its valid range.
Voice normalize(const Voice& voice);

// All voices in a dump, unpacked (32 for bulk dumps, 1 for single).
std::vector<Voice> voices(const Dump& dump);

// The voice name with trailing spaces removed and non-printable characters replaced.
std::string name(const Voice& voice);

// Builds a 4104-byte bulk dump (MIDI channel 1) from exactly 32 packed voices.
std::vector<uint8_t> makeBulkDump(std::span<const PackedVoice, 32> voices);

// Builds a 163-byte single-voice dump (MIDI channel 1).
std::vector<uint8_t> makeSingleDump(const Voice& voice);

}  // namespace ion::syx
