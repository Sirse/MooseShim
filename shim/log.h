#pragma once

#include <string>
#include <windows.h>

namespace shim {

enum class Level : int { off = 0, error = 1, info = 2, debug = 3, trace = 4 };

// Minimal, dependency-free logger. Line format follows the SDA CommHandler
// trace format so a captured exchange remains easy to inspect.
class Log {
public:
  static Log &instance();

  void open(const std::string &path, Level level);
  void close();

  bool enabled(Level l) const { return file_ != INVALID_HANDLE_VALUE && l <= level_; }

  // Writes one already-formatted record: " HH:MM:SS <lvl> <product>\t<text>"
  void write(Level l, const std::string &text);
  void writef(Level l, const char *fmt, ...);

private:
  Log() = default;
  ~Log();
  Log(const Log &) = delete;
  Log &operator=(const Log &) = delete;

  HANDLE file_ = INVALID_HANDLE_VALUE;
  Level level_ = Level::trace;
  CRITICAL_SECTION cs_{};
  bool cs_ready_ = false;
};

Level parseLevel(const std::string &s);

// Formats a PASSTHRU_MSG the way SDA does:
// "[ ProtocolId:%lu RxStatus:%lu TxStatus:%lu TimeStamp:%lu ExtraDataIndex:%lu
//    DataSize:%lu Data:xx xx ...]"
std::string formatMsg(const void *msg);

std::string formatHex(const unsigned char *data, unsigned long size);

} // namespace shim

#define SHIM_LOG(level, ...)                                                   \
  do {                                                                         \
    if (::shim::Log::instance().enabled(level))                                \
      ::shim::Log::instance().writef(level, __VA_ARGS__);                      \
  } while (0)

#define SHIM_TRACE(...) SHIM_LOG(::shim::Level::trace, __VA_ARGS__)
#define SHIM_DEBUG(...) SHIM_LOG(::shim::Level::debug, __VA_ARGS__)
#define SHIM_INFO(...) SHIM_LOG(::shim::Level::info, __VA_ARGS__)
#define SHIM_ERROR(...) SHIM_LOG(::shim::Level::error, __VA_ARGS__)
