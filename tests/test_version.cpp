#include <doctest.h>

#include <string>

#include "core/version.h"

TEST_CASE("version line has the documented shape") {
  const std::string line = ion::versionLine();
  CHECK(line.rfind(std::string("ion ") + ion::versionNumber() + " (", 0) == 0);
  CHECK(line.back() == ')');
  CHECK(line.find(", 20") != std::string::npos);  // build date
}
