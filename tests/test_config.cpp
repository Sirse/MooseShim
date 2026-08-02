#include "doctest.h"

#include "../shim/config.h"

#include <fstream>
#include <string>
#include <windows.h>

namespace {

class TempIni {
public:
  explicit TempIni(const std::string &contents) {
    char directory[MAX_PATH] = {};
    REQUIRE(GetTempPathA(sizeof(directory), directory) != 0);
    REQUIRE(GetTempFileNameA(directory, "msh", 0, path_) != 0);
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file << contents;
    REQUIRE(file.good());
  }

  ~TempIni() { DeleteFileA(path_); }

  const char *path() const { return path_; }

private:
  char path_[MAX_PATH] = {};
};

shim::Config loadIni(const std::string &contents) {
  TempIni file(contents);
  std::string error;
  const shim::Config config = shim::Config::load(file.path(), &error);
  REQUIRE(error.empty());
  return config;
}

} // namespace

TEST_CASE("adapter profiles apply defaults and preserve explicit overrides") {
  const shim::Config generic = loadIni(
      "[target]\n"
      "adapter_profile=generic\n"
      "[channel]\n"
      "ms_fallback=1\n"
      "emulate_clear_rx_buffer=1\n"
      "tolerate_unacked_functional_broadcasts=1\n"
      "normalize_empty_blocking_read=1\n");
  CHECK(generic.adapterProfile == shim::AdapterProfile::generic);
  CHECK(generic.msFallback);
  CHECK(generic.emulateClearRxBuffer);
  CHECK(generic.tolerateUnackedFunctionalBroadcasts);
  CHECK(generic.normalizeEmptyBlockingRead);

  const shim::Config mongoose = loadIni(
      "[target]\n"
      "adapter_profile=mongoose\n");
  CHECK(mongoose.adapterProfile == shim::AdapterProfile::mongoose);
  CHECK(mongoose.msFallback);
  CHECK(mongoose.emulateClearRxBuffer);
  CHECK(mongoose.tolerateUnackedFunctionalBroadcasts);
  CHECK(mongoose.normalizeEmptyBlockingRead);

  const shim::Config automatic = loadIni(
      "[target]\n"
      "adapter_profile=auto\n");
  CHECK(automatic.adapterProfile == shim::AdapterProfile::autoDetect);
  CHECK(automatic.msFallback);
  CHECK(automatic.emulateClearRxBuffer);
  CHECK_FALSE(automatic.tolerateUnackedFunctionalBroadcasts);
  CHECK(automatic.normalizeEmptyBlockingRead);
}
