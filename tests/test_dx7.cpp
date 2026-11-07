#include <doctest.h>

#include <string>

#include "engine/fm/dx7_voice.h"

using namespace ion;

namespace {

std::string ops(uint8_t mask) {
  std::string s;
  for (int op = 6; op >= 1; --op)
    if (mask & (1u << op)) s += std::to_string(op);
  return s;
}

// "6>5 5>4 4>3 2>1 | out 31 | fb 6>6"
std::string summary(int alg) {
  const dx7::Topology& t = dx7::topology(alg);
  std::string s;
  for (int op = 6; op >= 1; --op)
    for (int m = 6; m >= 1; --m)
      if (t.modulators[static_cast<std::size_t>(op)] & (1u << m)) s += std::to_string(m) + ">" + std::to_string(op) + " ";
  s += "| out " + ops(t.carriers) + " | fb " + std::to_string(t.feedbackFrom) + ">" + std::to_string(t.feedbackTo);
  return s;
}

}  // namespace

TEST_CASE("dx7: algorithm topology matches the DX7 chart") {
  // Checked against the Yamaha DX7 algorithm chart.
  CHECK(summary(0) == "6>5 5>4 4>3 2>1 | out 31 | fb 6>6");
  CHECK(summary(1) == "6>5 5>4 4>3 2>1 | out 31 | fb 2>2");
  CHECK(summary(3) == "6>5 5>4 3>2 2>1 | out 41 | fb 4>6");
  CHECK(summary(4) == "6>5 4>3 2>1 | out 531 | fb 6>6");
  CHECK(summary(5) == "6>5 4>3 2>1 | out 531 | fb 5>6");
  CHECK(summary(15) == "6>5 4>3 5>1 3>1 2>1 | out 1 | fb 6>6");
  CHECK(summary(21) == "6>5 6>4 6>3 2>1 | out 5431 | fb 6>6");
  CHECK(summary(31) == "| out 654321 | fb 6>6");
  for (int a = 0; a < dx7::kAlgorithms; ++a) {
    const dx7::Topology& t = dx7::topology(a);
    CHECK(t.carriers != 0);
    CHECK(t.feedbackFrom >= 1);
    CHECK(t.feedbackTo >= 1);
    MESSAGE("alg " << a + 1 << ": " << summary(a));
  }
}
