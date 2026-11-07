#include <doctest.h>

#include <array>
#include <cstring>
#include <random>

#include "library/syx.h"

using namespace ion;

namespace {

// Maximum of every unpacked voice byte (mirrors the table in syx.cpp, written
// out independently so the test checks it).
syx::Voice maxima() {
  syx::Voice m{};
  const uint8_t op[21] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 3, 3, 7, 3, 7, 99, 1, 31, 99, 14};
  for (int o = 0; o < 6; ++o)
    for (int i = 0; i < 21; ++i) m[static_cast<std::size_t>(o * 21 + i)] = op[i];
  const uint8_t g[19] = {99, 99, 99, 99, 99, 99, 99, 99, 31, 7, 1, 99, 99, 99, 99, 1, 5, 7, 48};
  for (int i = 0; i < 19; ++i) m[static_cast<std::size_t>(126 + i)] = g[i];
  for (int i = 0; i < 10; ++i) m[static_cast<std::size_t>(145 + i)] = 126;
  return m;
}

// A random but spec-valid voice. Generated, so the test needs no copyrighted bank.
syx::Voice randomVoice(std::mt19937& rng, int index) {
  const syx::Voice m = maxima();
  syx::Voice v{};
  for (std::size_t i = 0; i < 145; ++i) v[i] = static_cast<uint8_t>(std::uniform_int_distribution<int>(0, m[i])(rng));
  char name[11];
  std::snprintf(name, sizeof name, "TEST %-5d", index);
  std::memcpy(v.data() + 145, name, 10);
  return v;
}

std::array<syx::PackedVoice, 32> randomBank(uint32_t seed) {
  std::mt19937 rng(seed);
  std::array<syx::PackedVoice, 32> bank;
  for (int i = 0; i < 32; ++i) bank[static_cast<std::size_t>(i)] = syx::pack(randomVoice(rng, i));
  return bank;
}

}  // namespace

TEST_CASE("checksum matches the Yamaha definition") {
  const std::vector<uint8_t> data = {0x01, 0x02, 0x7F};
  // sum = 0x82; two's complement & 0x7F = 0x7E
  CHECK(syx::checksum(data) == 0x7E);
  const std::vector<uint8_t> zeros(4096, 0);
  CHECK(syx::checksum(zeros) == 0);
}

TEST_CASE("unpack then pack is byte-exact for spec-valid voices") {
  std::mt19937 rng(1234);
  for (int i = 0; i < 500; ++i) {
    const syx::Voice v = randomVoice(rng, i);
    const syx::PackedVoice p = syx::pack(v);
    CHECK(syx::unpack(p) == v);
    CHECK(syx::pack(syx::unpack(p)) == p);
  }
}

TEST_CASE("pitch mod sensitivity keeps all three bits") {
  syx::Voice v{};
  std::memcpy(v.data() + 145, "PMS TEST  ", 10);
  v[143] = 7;
  const auto p = syx::pack(v);
  CHECK((p[116] >> 4) == 7);
  CHECK(syx::unpack(p)[143] == 7);
}

TEST_CASE("bulk dump: build, parse and round-trip") {
  const auto bank = randomBank(42);
  const auto dump = syx::makeBulkDump(bank);
  REQUIRE(dump.size() == syx::kBulkDumpSize);
  CHECK(dump[0] == 0xF0);
  CHECK(dump.back() == 0xF7);

  const auto parsed = syx::parse(dump);
  REQUIRE(parsed.dumps.size() == 1);
  CHECK(parsed.warnings.empty());
  const auto& d = parsed.dumps[0];
  CHECK(d.kind == syx::DumpKind::Bulk);
  CHECK(d.checksum_ok);
  CHECK(d.terminated);

  const auto voices = syx::voices(d);
  REQUIRE(voices.size() == 32);
  std::array<syx::PackedVoice, 32> repacked;
  for (std::size_t i = 0; i < 32; ++i) repacked[i] = syx::pack(voices[i]);
  CHECK(syx::makeBulkDump(repacked) == dump);
  CHECK(syx::name(voices[3]) == "TEST 3");
}

TEST_CASE("single-voice dump round-trips") {
  std::mt19937 rng(7);
  const auto v = randomVoice(rng, 0);
  const auto dump = syx::makeSingleDump(v);
  REQUIRE(dump.size() == syx::kSingleDumpSize);
  const auto parsed = syx::parse(dump);
  REQUIRE(parsed.dumps.size() == 1);
  CHECK(parsed.dumps[0].kind == syx::DumpKind::Single);
  CHECK(parsed.dumps[0].checksum_ok);
  CHECK(syx::voices(parsed.dumps[0])[0] == v);
}

TEST_CASE("parser tolerates real-world damage") {
  const auto bank = randomBank(9);
  auto dump = syx::makeBulkDump(bank);

  SUBCASE("bad checksum: still parsed, flagged") {
    dump[100] ^= 0x01;
    const auto parsed = syx::parse(dump);
    REQUIRE(parsed.dumps.size() == 1);
    CHECK_FALSE(parsed.dumps[0].checksum_ok);
    CHECK_FALSE(parsed.warnings.empty());
  }
  SUBCASE("missing F7 (as in the 600 Voices .snd files)") {
    dump.pop_back();
    dump.push_back(0x00);
    dump.push_back(0x00);
    const auto parsed = syx::parse(dump);
    REQUIRE(parsed.dumps.size() == 1);
    CHECK(parsed.dumps[0].checksum_ok);
    CHECK_FALSE(parsed.dumps[0].terminated);
  }
  SUBCASE("another sysex message after the bulk dump") {
    const std::vector<uint8_t> extra = {0xF0, 0x43, 0x00, 0x06, 0x08, 0x60, 0x01, 0x02, 0xF7};
    dump.insert(dump.end(), extra.begin(), extra.end());
    const auto parsed = syx::parse(dump);
    CHECK(parsed.dumps.size() == 1);
  }
  SUBCASE("two bulk dumps in one file") {
    const auto second = syx::makeBulkDump(randomBank(10));
    dump.insert(dump.end(), second.begin(), second.end());
    CHECK(syx::parse(dump).dumps.size() == 2);
  }
  SUBCASE("headerless 4096-byte file") {
    const std::vector<uint8_t> raw(dump.begin() + 6, dump.begin() + 6 + 4096);
    const auto parsed = syx::parse(raw);
    REQUIRE(parsed.dumps.size() == 1);
    CHECK(parsed.dumps[0].kind == syx::DumpKind::Headerless);
    CHECK(syx::voices(parsed.dumps[0])[0] == syx::unpack(bank[0]));
  }
  SUBCASE("truncated") {
    dump.resize(2000);
    const auto parsed = syx::parse(dump);
    CHECK(parsed.dumps.empty());
    CHECK_FALSE(parsed.warnings.empty());
  }
  SUBCASE("garbage") {
    std::vector<uint8_t> junk(5000);
    std::mt19937 rng(3);
    for (auto& b : junk) b = static_cast<uint8_t>(rng());
    CHECK_NOTHROW(syx::parse(junk));
  }
}

TEST_CASE("unpack masks don't-care bits and clamps out-of-range values") {
  syx::PackedVoice p{};
  p.fill(0xFF);  // every bit set: the worst a corrupt cart can do
  const syx::Voice v = syx::unpack(p);
  const syx::Voice m = maxima();
  for (std::size_t i = 0; i < 145; ++i) CHECK(v[i] <= m[i]);
  for (std::size_t i = 145; i < 155; ++i) {
    CHECK(v[i] >= 32);
    CHECK(v[i] <= 126);
  }
}
