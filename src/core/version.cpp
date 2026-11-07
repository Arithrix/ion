#include "core/version.h"

#include "ion_version.h"

namespace ion {

const char* versionNumber() { return ION_VERSION_STRING; }

std::string versionLine() {
  std::string line = "ion " ION_VERSION_STRING " (" ION_GIT_COMMIT;
  if (ION_GIT_DIRTY) line += "-dirty";
  line += ", " ION_BUILD_DATE ")";
  return line;
}

}  // namespace ion
