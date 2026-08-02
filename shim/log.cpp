#include "log.h"

#include "include/J2534_v0404.h"
#include "product.h"

#include <cstdarg>
#include <cstdio>

namespace shim {

namespace {
constexpr unsigned long long kMaxLogBytes = 10ULL * 1024ULL * 1024ULL;
constexpr unsigned int kLogGenerations = 5;
constexpr unsigned long kMaxPayloadBytes = 64;
}

Log &Log::instance() {
  static Log log;
  return log;
}

Log::~Log() { close(); }

void Log::open(const std::string &path, Level level) {
  close();
  level_ = level;
  if (level == Level::off || path.empty())
    return;

  if (!cs_ready_) {
    InitializeCriticalSection(&cs_);
    cs_ready_ = true;
  }

  // Keep five previous generations. Handles are opened with FILE_SHARE_DELETE,
  // so a newly started SDA can rotate even while a diagnostic process still
  // has the old generation open.
  WIN32_FILE_ATTRIBUTE_DATA attrs{};
  if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs)) {
    const unsigned long long size =
        (static_cast<unsigned long long>(attrs.nFileSizeHigh) << 32) |
        attrs.nFileSizeLow;
    if (size >= kMaxLogBytes) {
      const std::string oldest = path + "." + std::to_string(kLogGenerations);
      DeleteFileA(oldest.c_str());
      for (unsigned int generation = kLogGenerations - 1; generation > 0;
           --generation) {
        const std::string from = path + "." + std::to_string(generation);
        const std::string to = path + "." + std::to_string(generation + 1);
        MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
      }
      MoveFileExA(path.c_str(), (path + ".1").c_str(),
                  MOVEFILE_REPLACE_EXISTING);
    }
  }

  // Full sharing on purpose. Two processes can hold the shim at once - SDA plus
  // a diagnostic run - and FILE_APPEND_DATA writes are atomic between them on
  // Windows. Without FILE_SHARE_WRITE the second process silently got no log at
  // all, which is the one thing that must keep working when something is wrong.
  file_ = CreateFileA(path.c_str(), FILE_APPEND_DATA,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) {
    char why[256];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                SHIM_PRODUCT_NAME ": cannot open log [%s], GetLastError=%lu\n",
                path.c_str(), ::GetLastError());
    OutputDebugStringA(why);
    return;
  }

  SetFilePointer(file_, 0, nullptr, FILE_END);
  // The build stamp is here so a log sent in a bug report identifies itself.
  writef(Level::info, "==== " SHIM_PRODUCT_NAME " " SHIM_VERSION_STR
                      " log opened (built " __DATE__ ") ====");
}

void Log::close() {
  const bool hadLock = cs_ready_;
  cs_ready_ = false;
  if (hadLock)
    EnterCriticalSection(&cs_);
  if (file_ != INVALID_HANDLE_VALUE) {
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
  }
  if (hadLock) {
    LeaveCriticalSection(&cs_);
    DeleteCriticalSection(&cs_);
  }
}

void Log::write(Level l, const std::string &text) {
  if (file_ == INVALID_HANDLE_VALUE || l > level_)
    return;

  SYSTEMTIME st;
  GetLocalTime(&st);

  char head[64];
  const int written_ = _snprintf_s(head, sizeof(head), _TRUNCATE,
                                   " %02u:%02u:%02u %d " SHIM_PRODUCT_NAME "\t",
                                   st.wHour,
                                   st.wMinute, st.wSecond, static_cast<int>(l));
  // _snprintf_s returns -1 when it truncates; treat that as an empty header
  // rather than turning it into a huge size_t.
  const size_t n = written_ > 0 ? static_cast<size_t>(written_) : 0;

  std::string line;
  line.reserve(n + text.size() + 2);
  line.assign(head, n);
  line += text;
  line += "\r\n";

  // close() clears cs_ready_ before deleting the section, so a late writer
  // degrades to an unsynchronised (but valid) write instead of touching a
  // destroyed lock.
  const bool locked = cs_ready_;
  if (locked)
    EnterCriticalSection(&cs_);
  DWORD written = 0;
  if (file_ != INVALID_HANDLE_VALUE)
    WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  if (locked)
    LeaveCriticalSection(&cs_);
}

void Log::writef(Level l, const char *fmt, ...) {
  if (file_ == INVALID_HANDLE_VALUE || l > level_)
    return;

  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  const int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);
  write(l, std::string(buf, n > 0 ? static_cast<size_t>(n) : 0));
}

Level parseLevel(const std::string &s) {
  if (s == "off")
    return Level::off;
  if (s == "error")
    return Level::error;
  if (s == "info")
    return Level::info;
  if (s == "debug")
    return Level::debug;
  return Level::trace;
}

std::string formatHex(const unsigned char *data, unsigned long size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  if (!data || !size)
    return out;
  out.reserve(size * 3);
  for (unsigned long i = 0; i < size; ++i) {
    if (i)
      out += ' ';
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 0x0f];
  }
  return out;
}

std::string formatMsg(const void *msgPtr) {
  if (!msgPtr)
    return "[ <null> ]";

  const PASSTHRU_MSG *m = static_cast<const PASSTHRU_MSG *>(msgPtr);
  unsigned long size = m->DataSize;
  if (size > sizeof(m->Data))
    size = sizeof(m->Data);
  const unsigned long available = size;
  if (size > kMaxPayloadBytes)
    size = kMaxPayloadBytes;

  char head[256];
  _snprintf_s(head, sizeof(head), _TRUNCATE,
              "[ ProtocolId:%lu RxStatus:%lu TxStatus:%lu TimeStamp:%lu "
              "ExtraDataIndex:%lu DataSize:%lu",
              m->ProtocolID, m->RxStatus, m->TxFlags, m->Timestamp,
              m->ExtraDataIndex, m->DataSize);

  std::string out(head);
  if (size) {
    out += " Data:";
    out += formatHex(m->Data, size);
  }
  if (available > size) {
    char omitted[64];
    _snprintf_s(omitted, sizeof(omitted), _TRUNCATE, " ...(+%lu bytes)",
                available - size);
    out += omitted;
  }
  out += "]";
  return out;
}

} // namespace shim
