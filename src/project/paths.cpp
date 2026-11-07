#include "project/paths.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace ion {
namespace fs = std::filesystem;

namespace {

fs::path envPath(const char* name, const fs::path& fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? fs::path(v) : fallback;
}

}  // namespace

Paths Paths::defaults() {
  const fs::path home = envPath("HOME", "/tmp");
  Paths p;
  p.configFile = envPath("XDG_CONFIG_HOME", home / ".config") / "ion" / "config.toml";
  p.dataDir = envPath("XDG_DATA_HOME", home / ".local" / "share") / "ion";
  p.projectsDir = home / "ion" / "projects";
  p.exportsDir = home / "ion" / "exports";
  p.kitsDir = home / "ion" / "kits";
  return p;
}

Paths Paths::under(const fs::path& root) {
  Paths p;
  p.configFile = root / "config" / "config.toml";
  p.dataDir = root / "data";
  p.projectsDir = root / "projects";
  p.exportsDir = root / "exports";
  p.kitsDir = root / "kits";
  return p;
}

fs::path expandHome(const std::string& p) {
  if (p == "~" || p.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "") / p.substr(p.size() > 1 ? 2 : 1);
  }
  return p;
}

std::string abbreviateHome(const std::string& p) {
  const char* home = std::getenv("HOME");
  if (!home || !*home) return p;
  const std::string h = home;
  if (p == h) return "~";
  if (p.size() > h.size() && p.rfind(h, 0) == 0 && p[h.size()] == '/') return "~" + p.substr(h.size());
  return p;
}

bool writeFileAtomic(const fs::path& path, const std::string& content, std::string& error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "can't create " + path.parent_path().string() + ": " + ec.message();
    return false;
  }
  const fs::path tmp = path.string() + ".tmp-" + std::to_string(getpid());
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      error = "can't write " + tmp.string();
      return false;
    }
    f << content;
    f.flush();
    if (!f) {
      error = "write failed: " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    error = "can't replace " + path.string() + ": " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool readFile(const fs::path& path, std::string& content) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  content = ss.str();
  return true;
}

bool validProjectName(const std::string& name) {
  if (name.empty() || name.size() > 64 || name == "." || name == "..") return false;
  for (unsigned char c : name)
    if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ' ')) return false;
  return name.front() != ' ' && name.back() != ' ';
}

}  // namespace ion
