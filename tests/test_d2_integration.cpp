#include "doctest.h"

#include "product.h"
#include "../shim/include/DiCE.hpp"
#include "../shim/include/J2534_v0404.h"
#include "../shim/d2/d2_transport.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <windows.h>

namespace {

using Open = long(WINAPI *)(void *, unsigned long *);
using Close = long(WINAPI *)(unsigned long);
using Connect = long(WINAPI *)(unsigned long, unsigned long, unsigned long,
                               unsigned long, unsigned long *);
using Disconnect = long(WINAPI *)(unsigned long);
using ReadMsgs = long(WINAPI *)(unsigned long, void *, unsigned long *, unsigned long);
using WriteMsgs = long(WINAPI *)(unsigned long, void *, unsigned long *, unsigned long);
using StartPeriodic = long(WINAPI *)(unsigned long, void *, unsigned long *, unsigned long);
using StopPeriodic = long(WINAPI *)(unsigned long, unsigned long);
using Ioctl = long(WINAPI *)(unsigned long, unsigned long, void *, void *);
using StartFilter = long(WINAPI *)(unsigned long, unsigned long, void *, void *,
                                   void *, unsigned long *);
using StopFilter = long(WINAPI *)(unsigned long, unsigned long);
using GetCallLog = long(WINAPI *)(char *, unsigned long *);
using Reset = long(WINAPI *)();
using PushRx = long(WINAPI *)(unsigned long, const unsigned char *, unsigned long);
using SetMode = long(WINAPI *)(unsigned long);

struct Api {
  HMODULE shim = nullptr;
  HMODULE mock = nullptr;
  Open open = nullptr;
  Close close = nullptr;
  Connect connect = nullptr;
  Disconnect disconnect = nullptr;
  ReadMsgs read = nullptr;
  WriteMsgs write = nullptr;
  StartPeriodic startPeriodic = nullptr;
  StopPeriodic stopPeriodic = nullptr;
  Ioctl ioctl = nullptr;
  StartFilter startFilter = nullptr;
  StopFilter stopFilter = nullptr;
  GetCallLog callLog = nullptr;
  Reset reset = nullptr;
  PushRx pushRx = nullptr;
  SetMode setMode = nullptr;
};

template <typename T> T bind(HMODULE module, const char *name) {
  return reinterpret_cast<T>(GetProcAddress(module, name));
}

const Api &api() {
  static Api result;
  static bool initialized = false;
  if (initialized)
    return result;
  SetEnvironmentVariableA(SHIM_ENV_TARGET, "mockj2534.dll");
  SetEnvironmentVariableA(SHIM_ENV_D2_ROUTE, "can");
  result.shim = LoadLibraryA(SHIM_MODULE_NAME);
  REQUIRE(result.shim != nullptr);
  result.mock = LoadLibraryA("mockj2534.dll");
  REQUIRE(result.mock != nullptr);
#define BIND(module, field, name)                                               \
  result.field = bind<decltype(result.field)>(module, name);                   \
  REQUIRE(result.field != nullptr)
  BIND(result.shim, open, "PassThruOpen");
  BIND(result.shim, close, "PassThruClose");
  BIND(result.shim, connect, "PassThruConnect");
  BIND(result.shim, disconnect, "PassThruDisconnect");
  BIND(result.shim, read, "PassThruReadMsgs");
  BIND(result.shim, write, "PassThruWriteMsgs");
  BIND(result.shim, startPeriodic, "PassThruStartPeriodicMsg");
  BIND(result.shim, stopPeriodic, "PassThruStopPeriodicMsg");
  BIND(result.shim, ioctl, "PassThruIoctl");
  BIND(result.shim, startFilter, "PassThruStartMsgFilter");
  BIND(result.shim, stopFilter, "PassThruStopMsgFilter");
  BIND(result.mock, callLog, "MockGetCallLog");
  BIND(result.mock, reset, "MockReset");
  BIND(result.mock, pushRx, "MockPushRx");
  BIND(result.mock, setMode, "MockSetMode");
#undef BIND
  initialized = true;
  return result;
}

PASSTHRU_MSG message(unsigned long protocol, std::initializer_list<unsigned char> data) {
  PASSTHRU_MSG result = {};
  result.ProtocolID = protocol;
  result.DataSize = static_cast<unsigned long>(data.size());
  std::copy(data.begin(), data.end(), result.Data);
  return result;
}

std::string callLog() {
  const Api &a = api();
  unsigned long size = 0;
  a.callLog(nullptr, &size);
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::vector<char> buffer(size, 0);
    const long status = a.callLog(buffer.data(), &size);
    if (status == STATUS_NOERROR)
      return std::string(buffer.data());
    REQUIRE(status == ERR_BUFFER_FULL);
  }
  FAIL("mock call log kept growing during eight snapshot attempts");
  return {};
}

void pushFrame(const Api &a, uint32_t canId, const shim::d2::D2CanFrame &frame) {
  unsigned char raw[4 + shim::d2::kCanPayloadSize] = {};
  raw[0] = static_cast<unsigned char>((canId >> 24) & 0xFF);
  raw[1] = static_cast<unsigned char>((canId >> 16) & 0xFF);
  raw[2] = static_cast<unsigned char>((canId >> 8) & 0xFF);
  raw[3] = static_cast<unsigned char>(canId & 0xFF);
  std::copy(frame.data.begin(), frame.data.begin() + frame.dataSize, raw + 4);
  REQUIRE(a.pushRx(CAN, raw, 4 + frame.dataSize) == STATUS_NOERROR);
}

std::vector<shim::d2::D2CanFrame> frames(uint32_t canId, size_t size) {
  shim::d2::D2LogicalMessage message;
  message.canId = canId;
  message.payload.resize(size);
  for (size_t i = 0; i < size; ++i)
    message.payload[i] = static_cast<unsigned char>(i & 0xFF);
  std::vector<shim::d2::D2CanFrame> result;
  REQUIRE(shim::d2::D2Segmenter::encode(message, &result));
  return result;
}

void pushLogical(const Api &a, uint32_t canId, size_t size) {
  for (const auto &frame : frames(canId, size))
    pushFrame(a, canId, frame);
}

void pushInterleaved(const Api &a, uint32_t firstId, uint32_t secondId,
                     size_t size) {
  const auto first = frames(firstId, size);
  const auto second = frames(secondId, size);
  const size_t count = std::max(first.size(), second.size());
  for (size_t i = 0; i < count; ++i) {
    if (i < first.size())
      pushFrame(a, firstId, first[i]);
    if (i < second.size())
      pushFrame(a, secondId, second[i]);
  }
}

PASSTHRU_MSG readOne(const Api &a, unsigned long channel,
                     unsigned long timeout = 1000) {
  PASSTHRU_MSG result = {};
  unsigned long count = 1;
  REQUIRE(a.read(channel, &result, &count, timeout) == STATUS_NOERROR);
  REQUIRE(count == 1);
  return result;
}

size_t occurrences(const std::string &text, const std::string &term) {
  size_t count = 0;
  size_t offset = 0;
  while ((offset = text.find(term, offset)) != std::string::npos) {
    ++count;
    offset += term.size();
  }
  return count;
}

struct D2Session {
  const Api &a = api();
  unsigned long device = 0;
  unsigned long channel = 0;

  D2Session() {
    REQUIRE(a.reset() == STATUS_NOERROR);
    REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
    REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                      500000, &channel) == STATUS_NOERROR);
  }
  ~D2Session() {
    a.disconnect(channel);
    a.close(device);
  }
};

} // namespace

TEST_CASE("D2 RX exposes padded short and full long logical payloads") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);
  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  pushLogical(a, 0x301, 8);
  const PASSTHRU_MSG eight = readOne(a, channel);
  CHECK(eight.DataSize == 12);
  CHECK(eight.Data[0] == 0x00);
  CHECK(eight.Data[2] == 0x03);
  CHECK(eight.Data[3] == 0x01);
  for (size_t i = 0; i < 8; ++i)
    CHECK(eight.Data[4 + i] == static_cast<unsigned char>(i));

  pushLogical(a, 0x302, 9);
  const PASSTHRU_MSG nine = readOne(a, channel);
  CHECK(nine.DataSize == 13);
  CHECK(nine.Data[3] == 0x02);
  for (size_t i = 0; i < 9; ++i)
    CHECK(nine.Data[4 + i] == static_cast<unsigned char>(i));

  pushLogical(a, 0x303, 64);
  const PASSTHRU_MSG sixtyFour = readOne(a, channel);
  CHECK(sixtyFour.DataSize == 68);
  CHECK(sixtyFour.Data[3] == 0x03);
  for (size_t i = 0; i < 64; ++i)
    CHECK(sixtyFour.Data[4 + i] == static_cast<unsigned char>(i));

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 RX defers a hard error after returning earlier messages") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);
  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  const unsigned char valid[] = {0x00, 0x00, 0x00, 0x03, 0xCA, 0x40, 0xB9};
  REQUIRE(a.pushRx(CAN, valid, sizeof(valid)) == STATUS_NOERROR);
  const unsigned char malformed[] = {0x00, 0x00, 0x00, 0x03, 0xCA, 0x40,
                                     0xB9, 0, 0, 0, 0, 0, 0};
  REQUIRE(a.pushRx(CAN, malformed, sizeof(malformed)) == STATUS_NOERROR);

  PASSTHRU_MSG output[2] = {};
  unsigned long count = 2;
  CHECK(a.read(channel, output, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK(output[0].DataSize == 12);
  CHECK(output[0].Data[3] == 0x03);
  CHECK(output[0].Data[4] == 0x40);
  CHECK(output[0].Data[5] == 0xB9);

  const unsigned char later[] = {0x00, 0x00, 0x00, 0x04, 0xCA, 0x41, 0xBA};
  REQUIRE(a.pushRx(CAN, later, sizeof(later)) == STATUS_NOERROR);
  const auto unfinished = frames(0x305, 64);
  pushFrame(a, 0x305, unfinished.front());
  PASSTHRU_MSG laterOutput[2] = {};
  count = 2;
  CHECK(a.read(channel, laterOutput, &count, 1500) == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK(laterOutput[0].Data[3] == 0x04);
  CHECK(laterOutput[0].Data[4] == 0x41);
  CHECK(laterOutput[0].Data[5] == 0xBA);

  PASSTHRU_MSG firstError = {};
  count = 1;
  CHECK(a.read(channel, &firstError, &count, 0) == ERR_INVALID_MSG);
  CHECK(count == 0);

  PASSTHRU_MSG secondError = {};
  count = 1;
  CHECK(a.read(channel, &secondError, &count, 0) == ERR_TIMEOUT);
  CHECK(count == 0);

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 RX accepts the 4095-byte maximum and rejects 4096 at segmentation") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);
  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  pushLogical(a, 0x304, 4095);
  const PASSTHRU_MSG maximum = readOne(a, channel, 15000);
  CHECK(maximum.DataSize == 4099);
  CHECK(maximum.Data[3] == 0x04);
  for (size_t i = 0; i < 4095; ++i)
    CHECK(maximum.Data[4 + i] == static_cast<unsigned char>(i & 0xFF));

  shim::d2::D2LogicalMessage oversized;
  oversized.canId = 0x305;
  oversized.payload.resize(4096);
  std::vector<shim::d2::D2CanFrame> rejected;
  CHECK_FALSE(shim::d2::D2Segmenter::encode(oversized, &rejected));

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 RX keeps two long assemblies independent by CAN ID") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);
  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  pushInterleaved(a, 0x306, 0x307, 64);
  const PASSTHRU_MSG first = readOne(a, channel);
  const PASSTHRU_MSG second = readOne(a, channel);
  CHECK(first.DataSize == 68);
  CHECK(second.DataSize == 68);
  CHECK(first.Data[3] == 0x06);
  CHECK(second.Data[3] == 0x07);
  for (const PASSTHRU_MSG *message : {&first, &second})
    for (size_t i = 0; i < 64; ++i)
      CHECK(message->Data[4 + i] == static_cast<unsigned char>(i));

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 vendor calls are translated to standard raw CAN") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);

  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  PASSTHRU_MSG tx = message(j2534::dice::CAN_XON_XOFF_STMIN,
                            {0x00, 0x0F, 0xFF, 0xFE, 0x40, 0xBE, 0x68, 0x25,
                             0x09, 0x85, 0x31, 0x03});
  unsigned long count = 1;
  REQUIRE(a.write(channel, &tx, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 1);

  PASSTHRU_MSG mask = message(j2534::dice::CAN_XON_XOFF_STMIN,
                              {0xFF, 0xFF, 0xFF, 0xFF});
  PASSTHRU_MSG pattern = message(j2534::dice::CAN_XON_XOFF_STMIN,
                              {0x00, 0x00, 0x00, 0x03});
  unsigned long filter = 0;
  REQUIRE(a.startFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &filter) ==
          STATUS_NOERROR);
  PASSTHRU_MSG secondPattern = message(j2534::dice::CAN_XON_XOFF_STMIN,
                                       {0x00, 0x00, 0x00, 0x04});
  unsigned long secondFilter = 0;
  REQUIRE(a.startFilter(channel, PASS_FILTER, &mask, &secondPattern, nullptr,
                        &secondFilter) == STATUS_NOERROR);

  const unsigned char unrelated[] = {0x00, 0x00, 0x00, 0x99, 0x0B, 0x01,
                                     0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  REQUIRE(a.pushRx(CAN, unrelated, sizeof(unrelated)) == STATUS_NOERROR);
  const unsigned char response[] = {0x00, 0x00, 0x00, 0x04, 0xCA, 0x40, 0xB9,
                                    0x00, 0x00, 0x00, 0x00, 0x00};
  REQUIRE(a.pushRx(CAN, response, sizeof(response)) == STATUS_NOERROR);
  PASSTHRU_MSG rx[2] = {};
  count = 2;
  REQUIRE(a.read(channel, &rx, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK(rx[0].ProtocolID == j2534::dice::CAN_XON_XOFF_STMIN);
  CHECK(rx[0].DataSize == 12);
  CHECK(rx[0].Data[3] == 0x04);
  CHECK(rx[0].Data[4] == 0x40);
  CHECK(rx[0].Data[5] == 0xB9);

  // A PASS filter selects the logical CAN id, but it is not proof that an
  // unknown wire header is a raw D2 response.
  const unsigned char unknownHeader[] = {0x00, 0x00, 0x00, 0x03, 0x00, 0x40};
  REQUIRE(a.pushRx(CAN, unknownHeader, sizeof(unknownHeader)) == STATUS_NOERROR);
  rx[0] = {};
  count = 1;
  CHECK(a.read(channel, &rx[0], &count, 0) == ERR_BUFFER_EMPTY);
  CHECK(count == 0);

  SCONFIG stmin = {j2534::dice::CAN_XON_XOFF_STMIN, 2};
  SCONFIG_LIST list = {1, &stmin};
  REQUIRE(a.ioctl(channel, SET_CONFIG, &list, nullptr) == STATUS_NOERROR);
  stmin.Value = 0;
  REQUIRE(a.ioctl(channel, GET_CONFIG, &list, nullptr) == STATUS_NOERROR);
  CHECK(stmin.Value == 2);

  const std::string log = callLog();
  CHECK(log.find("protocol=0x10000001") == std::string::npos);
  CHECK(log.find("protocol=0x00000005") != std::string::npos);
  CHECK(log.find("param=0x10000001") == std::string::npos);
  CHECK(log.find("8f 40 be 68 25 09 85 31") != std::string::npos);
  CHECK(log.find("49 03 00 00 00 00 00 00") != std::string::npos);

  CHECK(a.stopFilter(channel, filter) == STATUS_NOERROR);
  CHECK(a.stopFilter(channel, secondFilter) == STATUS_NOERROR);
  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 CAN_FRAME_RAW is sent as an unframed standard CAN message") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);

  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  PASSTHRU_MSG tx = message(j2534::dice::CAN_XON_XOFF_STMIN,
                            {0x00, 0x00, 0x00, 0x03, 0x40, 0xB9});
  tx.TxFlags = j2534::dice::CAN_FRAME_RAW;
  unsigned long count = 1;
  REQUIRE(a.write(channel, &tx, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 1);

  const std::string log = callLog();
  CHECK(log.find("protocol=0x00000005 txflags=0x00000100 size=6 data=00 00 00 03 40 b9") !=
        std::string::npos);
  CHECK(log.find("c8 40 b9") == std::string::npos);

  const unsigned char response[] = {0x00, 0x00, 0x00, 0x03, 0x40, 0xB9};
  REQUIRE(a.pushRx(CAN, response, sizeof(response)) == STATUS_NOERROR);
  PASSTHRU_MSG rx = {};
  count = 1;
  REQUIRE(a.read(channel, &rx, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK(rx.DataSize == 12);
  CHECK(rx.Data[4] == 0x40);
  CHECK(rx.Data[5] == 0xB9);
  CHECK(rx.Data[6] == 0);
  CHECK(rx.Data[11] == 0);

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 message TxFlags do not inherit Connect Flags") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);
  REQUIRE(a.setMode(2) == STATUS_NOERROR);

  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN,
                    CAN_29BIT_ID | PHYSICAL_CHANNEL, 500000, &channel) ==
          STATUS_NOERROR);

  PASSTHRU_MSG tx = message(j2534::dice::CAN_XON_XOFF_STMIN,
                            {0x00, 0x00, 0x00, 0x03, 0x40, 0xB9});
  unsigned long count = 1;
  REQUIRE(a.write(channel, &tx, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 1);

  const std::string log = callLog();
  CHECK(log.find("flags=0x20000100") != std::string::npos);
  CHECK(log.find("txflags=0x00000100") != std::string::npos);
  CHECK(log.find("txflags=0x20000100") == std::string::npos);

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
  CHECK(a.setMode(0) == STATUS_NOERROR);
}

TEST_CASE("D2 raw expectations survive a following transport write") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);

  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  PASSTHRU_MSG writes[2] = {
      message(j2534::dice::CAN_XON_XOFF_STMIN,
              {0x00, 0x00, 0x00, 0x03, 0x40, 0xB9}),
      message(j2534::dice::CAN_XON_XOFF_STMIN,
              {0x00, 0x00, 0x00, 0x03, 0x40, 0xB9})};
  writes[0].TxFlags = j2534::dice::CAN_FRAME_RAW;
  unsigned long count = 2;
  REQUIRE(a.write(channel, writes, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 2);

  const unsigned char rawResponse[] = {0x00, 0x00, 0x00, 0x03, 0x40, 0xB9};
  REQUIRE(a.pushRx(CAN, rawResponse, sizeof(rawResponse)) == STATUS_NOERROR);
  PASSTHRU_MSG rx = {};
  count = 1;
  REQUIRE(a.read(channel, &rx, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK(rx.DataSize == 12);
  CHECK(rx.Data[6] == 0);
  CHECK(rx.Data[11] == 0);

  const unsigned char transportResponse[] = {0x00, 0x00, 0x00, 0x03,
                                              0xCA, 0x40, 0xB9, 0, 0, 0, 0,
                                              0};
  REQUIRE(a.pushRx(CAN, transportResponse, sizeof(transportResponse)) ==
          STATUS_NOERROR);
  rx = {};
  count = 1;
  REQUIRE(a.read(channel, &rx, &count, 1000) == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK(rx.DataSize == 12);
  CHECK(rx.Data[4] == 0x40);
  CHECK(rx.Data[5] == 0xB9);
  CHECK(rx.Data[6] == 0);
  CHECK(rx.Data[11] == 0);

  const unsigned char unrelated[] = {0x00, 0x00, 0x00, 0x44, 0x40, 0x01};
  REQUIRE(a.pushRx(CAN, unrelated, sizeof(unrelated)) == STATUS_NOERROR);
  rx = {};
  count = 1;
  CHECK(a.read(channel, &rx, &count, 0) == ERR_BUFFER_EMPTY);
  CHECK(count == 0);

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 raw periodic messages stay unframed") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);

  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  PASSTHRU_MSG periodic = message(j2534::dice::CAN_XON_XOFF_STMIN,
                                  {0x00, 0x00, 0x00, 0x03, 0x40, 0xB9});
  periodic.TxFlags = j2534::dice::CAN_FRAME_RAW;
  unsigned long messageId = 0;
  REQUIRE(a.startPeriodic(channel, &periodic, &messageId, 1000) == STATUS_NOERROR);

  const std::string log = callLog();
  CHECK(log.find("StartPeriodicMsg") != std::string::npos);
  CHECK(log.find("protocol=0x00000005 txflags=0x00000100 size=6 data=00 00 00 03 40 b9") !=
        std::string::npos);
  CHECK(log.find("c8 40 b9") == std::string::npos);

  CHECK(a.stopPeriodic(channel, messageId) == STATUS_NOERROR);
  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 multi-frame periodic messages use a shim-owned scheduler") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);

  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  // This is the logical eight-byte periodic message from the SDA trace. It
  // must become the independent 0x8F/0x49 wire sequence.
  PASSTHRU_MSG periodic = message(
      j2534::dice::CAN_XON_XOFF_STMIN,
      {0x00, 0x0F, 0xFF, 0xFE, 0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00});
  unsigned long messageId = 0;
  REQUIRE(a.startPeriodic(channel, &periodic, &messageId, 20) == STATUS_NOERROR);
  CHECK((messageId & 0x80000000UL) != 0);
  Sleep(60);
  CHECK(a.stopPeriodic(channel, messageId) == STATUS_NOERROR);

  const std::string log = callLog();
  CHECK(log.find("StartPeriodicMsg") == std::string::npos);
  CHECK(log.find("data=00 0f ff fe 8f ff 86") != std::string::npos);
  CHECK(log.find("data=00 0f ff fe 49 00") != std::string::npos);

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 CLEAR_RX_BUFFER clears shim-owned receive state") {
  const Api &a = api();
  REQUIRE(a.reset() == STATUS_NOERROR);

  unsigned long device = 0;
  REQUIRE(a.open(nullptr, &device) == STATUS_NOERROR);
  unsigned long channel = 0;
  REQUIRE(a.connect(device, j2534::dice::CAN_XON_XOFF_STMIN, CAN_29BIT_ID,
                    500000, &channel) == STATUS_NOERROR);

  const unsigned char first[] = {0x00, 0x00, 0x00, 0x03, 0x8F, 1, 2, 3,
                                 4, 5, 6, 7};
  REQUIRE(a.pushRx(CAN, first, sizeof(first)) == STATUS_NOERROR);
  PASSTHRU_MSG rx = {};
  unsigned long count = 1;
  CHECK(a.read(channel, &rx, &count, 0) == ERR_BUFFER_EMPTY);
  CHECK(count == 0);

  REQUIRE(a.ioctl(channel, CLEAR_RX_BUFFER, nullptr, nullptr) == STATUS_NOERROR);

  const unsigned char last[] = {0x00, 0x00, 0x00, 0x03, 0x49, 8};
  REQUIRE(a.pushRx(CAN, last, sizeof(last)) == STATUS_NOERROR);
  rx = {};
  count = 1;
  CHECK(a.read(channel, &rx, &count, 0) == ERR_BUFFER_EMPTY);
  CHECK(count == 0);

  CHECK(a.disconnect(channel) == STATUS_NOERROR);
  CHECK(a.close(device) == STATUS_NOERROR);
}

TEST_CASE("D2 connect installs a target receive filter") {
  D2Session session;
  CHECK(callLog().find("StartMsgFilter") != std::string::npos);
}

TEST_CASE("D2 CLEAR_PERIODIC_MSGS stops shim schedulers") {
  D2Session session;
  PASSTHRU_MSG periodic = message(
      j2534::dice::CAN_XON_XOFF_STMIN,
      {0x00, 0x0F, 0xFF, 0xFE, 0xFF, 0x86, 0, 0, 0, 0, 0, 0});
  unsigned long id = 0;
  REQUIRE(session.a.startPeriodic(session.channel, &periodic, &id, 20) ==
          STATUS_NOERROR);
  Sleep(60);
  REQUIRE(session.a.ioctl(session.channel, CLEAR_PERIODIC_MSGS, nullptr, nullptr) ==
          STATUS_NOERROR);
  const size_t before = occurrences(callLog(), "WriteMsgs");
  Sleep(80);
  CHECK(occurrences(callLog(), "WriteMsgs") == before);
  CHECK(session.a.stopPeriodic(session.channel, id) == ERR_INVALID_MSG_ID);
}

TEST_CASE("D2 CLEAR_MSG_FILTERS invalidates logical filters") {
  D2Session session;
  PASSTHRU_MSG mask = message(j2534::dice::CAN_XON_XOFF_STMIN,
                              {0xFF, 0xFF, 0xFF, 0xFF});
  PASSTHRU_MSG pattern = message(j2534::dice::CAN_XON_XOFF_STMIN,
                                 {0x00, 0x00, 0x00, 0x03});
  unsigned long id = 0;
  REQUIRE(session.a.startFilter(session.channel, PASS_FILTER, &mask, &pattern,
                                nullptr, &id) == STATUS_NOERROR);
  REQUIRE(session.a.ioctl(session.channel, CLEAR_MSG_FILTERS, nullptr, nullptr) ==
          STATUS_NOERROR);
  CHECK(session.a.stopFilter(session.channel, id) == ERR_INVALID_FILTER_ID);
  CHECK(occurrences(callLog(), "StartMsgFilter") == 2);
}

TEST_CASE("blocking D2 reads do not starve scheduled writes") {
  D2Session session;
  PASSTHRU_MSG periodic = message(
      j2534::dice::CAN_XON_XOFF_STMIN,
      {0x00, 0x0F, 0xFF, 0xFE, 0xFF, 0x86, 0, 0, 0, 0, 0, 0});
  unsigned long id = 0;
  REQUIRE(session.a.startPeriodic(session.channel, &periodic, &id, 20) ==
          STATUS_NOERROR);
  Sleep(40);
  std::thread reader([&] {
    PASSTHRU_MSG rx = {};
    unsigned long count = 1;
    session.a.read(session.channel, &rx, &count, 250);
  });
  const size_t before = occurrences(callLog(), "WriteMsgs");
  Sleep(100);
  const size_t after = occurrences(callLog(), "WriteMsgs");
  reader.join();
  REQUIRE(session.a.stopPeriodic(session.channel, id) == STATUS_NOERROR);
  CHECK(after > before);
}

TEST_CASE("continuous filtered D2 traffic allows writes and periodic keepalives") {
  D2Session session;
  using ContinuousRx = long(WINAPI *)(const PASSTHRU_MSG *);
  const auto continuousRx = bind<ContinuousRx>(session.a.mock, "MockSetContinuousRx");
  REQUIRE(continuousRx != nullptr);
  PASSTHRU_MSG unrelated = message(CAN, {0, 0, 0, 0x99, 0xCA, 0x40, 0xB9});
  PASSTHRU_MSG mask = message(j2534::dice::CAN_XON_XOFF_STMIN, {0xff, 0xff, 0xff, 0xff});
  PASSTHRU_MSG pattern = message(j2534::dice::CAN_XON_XOFF_STMIN, {0, 0, 0, 3});
  unsigned long filterId = 0;
  REQUIRE(session.a.startFilter(session.channel, PASS_FILTER, &mask, &pattern,
                                nullptr, &filterId) == STATUS_NOERROR);
  REQUIRE(continuousRx(&unrelated) == STATUS_NOERROR);
  std::atomic<bool> finished{false};
  long readStatus = ERR_FAILED;
  unsigned long readCount = 1;
  std::thread reader([&] {
    PASSTHRU_MSG rx = {};
    readStatus = session.a.read(session.channel, &rx, &readCount, 500);
    finished.store(true);
  });
  for (int i = 0; i < 100 && callLog().find("ReadMsgs") == std::string::npos; ++i)
    Sleep(1);
  PASSTHRU_MSG request = message(j2534::dice::CAN_XON_XOFF_STMIN,
                                 {0, 0, 0, 3, 0x40, 0xB9});
  unsigned long count = 1;
  const long writeStatus = session.a.write(session.channel, &request, &count, 100);
  const bool writeDuringRead = !finished.load();
  PASSTHRU_MSG periodic = message(j2534::dice::CAN_XON_XOFF_STMIN,
      {0, 0x0f, 0xff, 0xfe, 0xff, 0x86, 0, 0, 0, 0, 0, 0});
  unsigned long periodicId = 0;
  const long periodicStatus = session.a.startPeriodic(session.channel, &periodic,
                                                       &periodicId, 20);
  const size_t before = occurrences(callLog(), "WriteMsgs");
  Sleep(80);
  const size_t after = occurrences(callLog(), "WriteMsgs");
  const bool periodicDuringRead = !finished.load();
  reader.join();
  continuousRx(nullptr);
  if (periodicStatus == STATUS_NOERROR)
    session.a.stopPeriodic(session.channel, periodicId);
  CHECK(writeStatus == STATUS_NOERROR);
  CHECK(writeDuringRead);
  CHECK(periodicStatus == STATUS_NOERROR);
  CHECK(periodicDuringRead);
  CHECK(after > before);
  CHECK(readStatus == ERR_TIMEOUT);
  CHECK(readCount == 0);
}

TEST_CASE("D2 periodic first frame respects the previous ordinary write STmin") {
  D2Session session;
  using GetWriteTime = long(WINAPI *)(unsigned long, unsigned long, ULONGLONG *);
  const auto getTime = bind<GetWriteTime>(session.a.mock, "MockGetWriteTime");
  REQUIRE(getTime != nullptr);
  SCONFIG stmin = {j2534::dice::CAN_XON_XOFF_STMIN, 100};
  SCONFIG_LIST list = {1, &stmin};
  REQUIRE(session.a.ioctl(session.channel, SET_CONFIG, &list, nullptr) == STATUS_NOERROR);
  PASSTHRU_MSG request = message(j2534::dice::CAN_XON_XOFF_STMIN,
                                 {0, 0, 0, 3, 0x40, 0xB9});
  unsigned long count = 1;
  REQUIRE(session.a.write(session.channel, &request, &count, 1000) == STATUS_NOERROR);
  PASSTHRU_MSG periodic = message(j2534::dice::CAN_XON_XOFF_STMIN,
      {0, 0x0f, 0xff, 0xfe, 0xff, 0x86, 0, 0, 0, 0, 0, 0});
  unsigned long periodicId = 0;
  REQUIRE(session.a.startPeriodic(session.channel, &periodic, &periodicId, 1000) == STATUS_NOERROR);
  ULONGLONG first = 0, second = 0, third = 0;
  REQUIRE(getTime(session.channel, 0, &first) == STATUS_NOERROR);
  long status = ERR_BUFFER_EMPTY;
  for (int i = 0; i < 1000 && status != STATUS_NOERROR; ++i) {
    status = getTime(session.channel, 2, &third);
    if (status != STATUS_NOERROR)
      Sleep(1);
  }
  session.a.stopPeriodic(session.channel, periodicId);
  REQUIRE(status == STATUS_NOERROR);
  REQUIRE(getTime(session.channel, 1, &second) == STATUS_NOERROR);
  // GetTickCount64 has coarse resolution; allow one tick of measurement error.
  CHECK(second - first >= 85);
  CHECK(third - second >= 85);
}

TEST_CASE("D2 STmin respects the WriteMsgs deadline") {
  D2Session session;
  PASSTHRU_MSG tx = message(j2534::dice::CAN_XON_XOFF_STMIN,
                            {0, 0, 0, 3, 0x40, 0xB9});
  SCONFIG stmin = {j2534::dice::CAN_XON_XOFF_STMIN, 200};
  SCONFIG_LIST list = {1, &stmin};
  REQUIRE(session.a.ioctl(session.channel, SET_CONFIG, &list, nullptr) ==
          STATUS_NOERROR);
  unsigned long count = 1;
  REQUIRE(session.a.write(session.channel, &tx, &count, 1000) == STATUS_NOERROR);
  const auto begin = std::chrono::steady_clock::now();
  count = 1;
  CHECK(session.a.write(session.channel, &tx, &count, 10) == ERR_TIMEOUT);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - begin);
  CHECK(elapsed < std::chrono::milliseconds(100));
  CHECK(count == 0);
}

TEST_CASE("D2 raw response lifetime is independent of write timeout") {
  D2Session session;
  PASSTHRU_MSG tx = message(j2534::dice::CAN_XON_XOFF_STMIN,
                            {0, 0, 0, 3, 0x40, 0xB9});
  tx.TxFlags = j2534::dice::CAN_FRAME_RAW;
  unsigned long count = 1;
  REQUIRE(session.a.write(session.channel, &tx, &count, 10) == STATUS_NOERROR);
  Sleep(25);
  const unsigned char response[] = {0, 0, 0, 3, 0x40, 0xB9};
  REQUIRE(session.a.pushRx(CAN, response, sizeof(response)) == STATUS_NOERROR);
  PASSTHRU_MSG rx = {};
  count = 1;
  CHECK(session.a.read(session.channel, &rx, &count, 30) == STATUS_NOERROR);
  CHECK(count == 1);
}
