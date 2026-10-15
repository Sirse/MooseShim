// Integration tests: the shim DLL is loaded exactly the way SDA's
// CommHandlerJ2534 loads it (LoadLibrary + GetProcAddress on undecorated
// names), aimed at mockj2534.dll, and driven through the sequences taken from
// the reference DiCE trace.
//
// Nothing here may ever run against a real adapter, which is why the target is
// pinned to the mock through the environment before the first call and asserted
// afterwards.

#include "doctest.h"

#include "../shim/include/DiCE.hpp"
#include "product.h"
#include "../shim/include/J2534_v0404.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <windows.h>

namespace {

typedef long(WINAPI *MOCKGETCALLLOG)(char *, unsigned long *);
typedef long(WINAPI *MOCKRESET)(void);
typedef long(WINAPI *MOCKSETMODE)(unsigned long);
typedef long(WINAPI *MOCKSETCONNECTFAILURE)(long);
typedef long(WINAPI *MOCKPUSHRX)(unsigned long, const unsigned char *, unsigned long);

struct Api {
  HMODULE m = nullptr;
  PTOPEN Open = nullptr;
  PTCLOSE Close = nullptr;
  PTCONNECT Connect = nullptr;
  PTDISCONNECT Disconnect = nullptr;
  PTREADMSGS ReadMsgs = nullptr;
  PTWRITEMSGS WriteMsgs = nullptr;
  PTSTARTPERIODICMSG StartPeriodicMsg = nullptr;
  PTSTOPPERIODICMSG StopPeriodicMsg = nullptr;
  PTSTARTMSGFILTER StartMsgFilter = nullptr;
  PTSTOPMSGFILTER StopMsgFilter = nullptr;
  PTSETPROGRAMMINGVOLTAGE SetProgrammingVoltage = nullptr;
  PTREADVERSION ReadVersion = nullptr;
  PTGETLASTERROR GetLastError_ = nullptr;
  PTIOCTL Ioctl = nullptr;
};

// One shim instance for the whole run. Loading it repeatedly would re-enter the
// INIT_ONCE, which is exactly what the production path never does.
const Api &shim() {
  static Api api;
  static bool ready = false;
  if (ready)
    return api;

  // Must be set before the shim initialises, which happens on its first export
  // call. Doing it here rather than in CMake means a developer running the test
  // binary directly is protected too.
  SetEnvironmentVariableA(SHIM_ENV_TARGET, "mockj2534.dll");

  api.m = LoadLibraryA(SHIM_MODULE_NAME);
  REQUIRE_MESSAGE(api.m != nullptr,
                  SHIM_MODULE_NAME " must sit next to the test binary");

#define BIND(field, name)                                                      \
  api.field = reinterpret_cast<decltype(api.field)>(GetProcAddress(api.m, name)); \
  REQUIRE_MESSAGE(api.field != nullptr, "undecorated export missing: " name);

  BIND(Open, "PassThruOpen")
  BIND(Close, "PassThruClose")
  BIND(Connect, "PassThruConnect")
  BIND(Disconnect, "PassThruDisconnect")
  BIND(ReadMsgs, "PassThruReadMsgs")
  BIND(WriteMsgs, "PassThruWriteMsgs")
  BIND(StartPeriodicMsg, "PassThruStartPeriodicMsg")
  BIND(StopPeriodicMsg, "PassThruStopPeriodicMsg")
  BIND(StartMsgFilter, "PassThruStartMsgFilter")
  BIND(StopMsgFilter, "PassThruStopMsgFilter")
  BIND(SetProgrammingVoltage, "PassThruSetProgrammingVoltage")
  BIND(ReadVersion, "PassThruReadVersion")
  BIND(GetLastError_, "PassThruGetLastError")
  BIND(Ioctl, "PassThruIoctl")
#undef BIND

  ready = true;
  return api;
}

template <typename Fn> Fn mockFn(const char *name) {
  HMODULE m = GetModuleHandleA("mockj2534.dll");
  REQUIRE_MESSAGE(m != nullptr, "the shim must have loaded mockj2534.dll, not a "
                                "real driver - refusing to continue");
  return reinterpret_cast<Fn>(GetProcAddress(m, name));
}

std::string callLog() {
  MOCKGETCALLLOG fn = mockFn<MOCKGETCALLLOG>("MockGetCallLog");
  unsigned long size = 0;
  fn(nullptr, &size);
  std::vector<char> buf(size + 1, 0);
  if (fn(buf.data(), &size) != STATUS_NOERROR)
    return {};
  return std::string(buf.data());
}

void resetMock() { mockFn<MOCKRESET>("MockReset")(); }

void setMockMode(unsigned long mode) {
  REQUIRE(mockFn<MOCKSETMODE>("MockSetMode")(mode) == STATUS_NOERROR);
}

struct MockModeScope {
  explicit MockModeScope(unsigned long mode) { setMockMode(mode); }
  ~MockModeScope() { mockFn<MOCKSETMODE>("MockSetMode")(0); }
};

void setMockConnectFailure(long status) {
  REQUIRE(mockFn<MOCKSETCONNECTFAILURE>("MockSetConnectFailure")(status) ==
          STATUS_NOERROR);
}

void pushRx(unsigned long protocol, const std::vector<unsigned char> &data) {
  mockFn<MOCKPUSHRX>("MockPushRx")(protocol, data.data(),
                                   static_cast<unsigned long>(data.size()));
}

bool logHas(const std::string &needle) {
  return callLog().find(needle) != std::string::npos;
}

size_t logCount(const std::string &needle) {
  const std::string log = callLog();
  size_t count = 0;
  for (size_t pos = 0; (pos = log.find(needle, pos)) != std::string::npos;
       pos += needle.size())
    ++count;
  return count;
}

// The shim keeps its log open, so it has to be read with full sharing.
std::string readShimLog() {
  // Same resolution the shim performs: [log] path from the INI next to the DLL,
  // %VARS% expanded, empty meaning MooseShim.log beside the DLL.
  std::string path;
  FILE *f = nullptr;
  if (fopen_s(&f, SHIM_INI_NAME, "rb") == 0 && f) {
    char line[512];
    bool inLog = false;
    while (fgets(line, sizeof(line), f)) {
      std::string s(line);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
      if (!s.empty() && s[0] == '[')
        inLog = (s == "[log]");
      else if (inLog && s.rfind("path=", 0) == 0)
        path = s.substr(5);
    }
    fclose(f);
  }
  if (path.empty())
    path = SHIM_PRODUCT_NAME ".log";
  char expanded[MAX_PATH * 2];
  const DWORD n =
      ExpandEnvironmentStringsA(path.c_str(), expanded, sizeof(expanded));
  if (n && n <= sizeof(expanded))
    path.assign(expanded, n - 1);

  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return {};
  std::string out;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
    out.append(buf, got);
  CloseHandle(h);
  return out;
}

PASSTHRU_MSG makeMsg(unsigned long protocol, unsigned long txFlags,
                     const std::vector<unsigned char> &data) {
  PASSTHRU_MSG m = {};
  m.ProtocolID = protocol;
  m.TxFlags = txFlags;
  m.DataSize = static_cast<unsigned long>(data.size());
  memcpy(m.Data, data.data(), data.size());
  return m;
}

long setConfig(unsigned long channel, std::vector<SCONFIG> params) {
  SCONFIG_LIST list;
  list.NumOfParams = static_cast<uint32_t>(params.size());
  list.ConfigPtr = params.data();
  return shim().Ioctl(channel, SET_CONFIG, &list, nullptr);
}

// Opens the device and starts from a clean call log.
unsigned long freshDevice() {
  unsigned long device = 0;
  REQUIRE(shim().Open(nullptr, &device) == STATUS_NOERROR);
  resetMock();
  return device;
}

} // namespace

TEST_CASE("the shim exports the v04.04 API undecorated and loads the target") {
  const Api &api = shim();
  CHECK(api.Open != nullptr);

  unsigned long device = 0;
  CHECK(api.Open(nullptr, &device) == STATUS_NOERROR);

  char fw[80] = {}, dll[80] = {}, apiver[80] = {};
  CHECK(api.ReadVersion(device, fw, dll, apiver) == STATUS_NOERROR);
  CHECK(std::string(apiver) == "04.04");
  CHECK(api.Close(device) == STATUS_NOERROR);
}

TEST_CASE("the host device name is not forwarded to the target") {
  // SDA passes the Name from its registry entry. The DrewTech driver reads
  // pName as a device selector and answers ERR_DEVICE_NOT_CONNECTED for it.
  resetMock();
  unsigned long device = 0;
  char hostName[] = SHIM_PRODUCT_NAME;
  CHECK(shim().Open(hostName, &device) == STATUS_NOERROR);
  CHECK_FALSE(logHas("Open name=" SHIM_PRODUCT_NAME));
  shim().Close(device);
}

TEST_CASE("standard SET_CONFIG parameters reach the target") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);

  CHECK(setConfig(ch, {{DATA_RATE, 500000}, {BIT_SAMPLE_POINT, 68}}) ==
        STATUS_NOERROR);
  CHECK(logHas("param=0x00000001 value=500000"));
  CHECK(logHas("param=0x00000017 value=68"));

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("the DiCE vendor SET_CONFIG parameters never reach the target") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);

  CHECK(setConfig(ch, {{j2534::dice::ISO15765_SWDL_FABRICATE, 1}}) ==
        STATUS_NOERROR);
  CHECK(setConfig(ch, {{j2534::dice::CAN_XON_XOFF_STMIN, 0}}) == STATUS_NOERROR);

  // A mixed list must still deliver its standard half.
  CHECK(setConfig(ch, {{DATA_RATE, 500000},
                       {j2534::dice::CAN_XON_XOFF_STMIN, 0},
                       {BIT_SAMPLE_POINT, 68}}) == STATUS_NOERROR);

  CHECK_FALSE(logHas("param=0x10000001"));
  CHECK_FALSE(logHas("param=0x10000002"));
  CHECK(logHas("param=0x00000001 value=500000"));

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("the DiCE vendor protocol is refused without touching the target") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  CHECK(shim().Connect(device, j2534::dice::CAN_XON_XOFF_STMIN, 0, 125000, &ch) ==
        ERR_INVALID_PROTOCOL_ID);
  CHECK_FALSE(logHas("protocol=0x10000001"));
  shim().Close(device);
}

TEST_CASE("a vendor ioctl is absorbed") {
  const unsigned long device = freshDevice();
  CHECK(shim().Ioctl(device, j2534::dice::DICE_CAN_XON_XOFF_FILTER, nullptr,
                     nullptr) == STATUS_NOERROR);
  CHECK_FALSE(logHas("id=0x20000001")); // == DrewTech DT_READ_DIO
  shim().Close(device);
}

TEST_CASE("FUNC_ADDR is stripped from Connect flags, PHYSICAL_CHANNEL is not") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  shim().Connect(device, ISO15765,
                 PHYSICAL_CHANNEL | CAN_29BIT_ID | j2534::dice::FUNC_ADDR, 125000,
                 &ch);
  CHECK_FALSE(logHas("flags=0x30000100"));
  CHECK(logHas("flags=0x20000100"));
  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("vendor TxFlags bits are stripped and the caller's buffer restored") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);

  // 0x20000040 is what SDA actually sends: bit 29 is defined by nobody, yet it
  // appears on more than half of the messages in the reference trace.
  PASSTHRU_MSG tx =
      makeMsg(ISO15765, 0x20000000UL | ISO15765_FRAME_PAD | j2534::dice::FUNC_ADDR,
              {0x00, 0x00, 0x07, 0xe0, 0x22, 0xf1, 0xa3});
  const unsigned long originalTxFlags = tx.TxFlags;
  unsigned long num = 1;
  CHECK(shim().WriteMsgs(ch, &tx, &num, 100) == STATUS_NOERROR);

  CHECK_MESSAGE(logHas("txflags=0x00000040"),
                "DiCE FUNC_ADDR must not enable ISO-TP extended addressing");
  CHECK_FALSE(logHas("txflags=0x20000040"));
  CHECK_FALSE(logHas("txflags=0x30000040"));
  CHECK_MESSAGE(tx.TxFlags == originalTxFlags,
                "the host's buffer must come back exactly as it was handed in");

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("GET_CONFIG is filtered and the standard answers are copied back") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);

  SCONFIG params[] = {{DATA_RATE, 0}, {j2534::dice::CAN_XON_XOFF_STMIN, 0}};
  SCONFIG_LIST list = {2, params};
  CHECK(shim().Ioctl(ch, GET_CONFIG, &list, nullptr) == STATUS_NOERROR);

  CHECK(params[0].Value == (0xC0DE0000u | DATA_RATE)); // the mock's marker
  CHECK(params[1].Value == 0);                         // absorbed, untouched
  CHECK_FALSE(logHas("param=0x10000001"));

  shim().Disconnect(ch);
  shim().Close(device);
}

// --- MS channel translation --------------------------------------------------
// The mock refuses PHYSICAL_CHANNEL the way MongoosePro JLR does, so these
// exercise the fallback rather than the direct path.

TEST_CASE("a native dual-CAN adapter keeps the host's direct MS route") {
  MockModeScope mode(2);
  const unsigned long device = freshDevice();
  unsigned long ch = 0;

  REQUIRE(shim().Connect(device, ISO15765,
                         PHYSICAL_CHANNEL | CAN_29BIT_ID, 125000, &ch) ==
          STATUS_NOERROR);
  CHECK(logCount("Connect device=") == 1);
  CHECK(logHas("protocol=0x00000006"));
  CHECK_FALSE(logHas("protocol=0x00008005"));
  CHECK_FALSE(logHas("param=0x00008001"));

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("Mongoose opens MS after HS despite the native channel being busy") {
  MockModeScope mode(0);
  const unsigned long device = freshDevice();
  unsigned long hs = 0, ms = 0;
  REQUIRE(shim().Connect(device, ISO15765, 0, 500000, &hs) == STATUS_NOERROR);
  REQUIRE(shim().Connect(device, ISO15765, PHYSICAL_CHANNEL, 125000, &ms) ==
          STATUS_NOERROR);
  CHECK(ms != hs);
  CHECK(ms != 0);
  CHECK(logCount("Connect device=") == 3);
  CHECK(logHas("protocol=0x00008005"));
  CHECK(logHas("param=0x00008001 value=779"));
  PASSTHRU_MSG tx = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                             {0, 0, 7, 0x20, 0x22, 0xf1, 0x91});
  unsigned long count = 1;
  REQUIRE(shim().WriteMsgs(ms, &tx, &count, 100) == STATUS_NOERROR);
  CHECK(logHas("protocol=0x00008005 txflags=0x00000040"));
  pushRx(ISO15765_PS, {0, 0, 7, 0x28, 0x62, 0xf1, 0x91, 0, 0});
  PASSTHRU_MSG rx = {};
  count = 1;
  REQUIRE(shim().ReadMsgs(ms, &rx, &count, 100) == STATUS_NOERROR);
  CHECK(rx.ProtocolID == ISO15765);
  CHECK(rx.Data[4] == 0x62);
  shim().Disconnect(ms);
  shim().Disconnect(hs);
  shim().Close(device);
}

TEST_CASE("busy-channel failure on a direct HS request is preserved") {
  const unsigned long device = freshDevice();
  setMockConnectFailure(ERR_CHANNEL_IN_USE);
  unsigned long channel = 123;
  CHECK(shim().Connect(device, ISO15765, 0, 500000, &channel) == ERR_CHANNEL_IN_USE);
  CHECK(channel == 0);
  CHECK(logCount("Connect device=") == 1);
  setMockConnectFailure(STATUS_NOERROR);
  shim().Close(device);
}

TEST_CASE("MS fallback does not hide a device-level connect failure") {
  MockModeScope mode(0);
  const unsigned long device = freshDevice();
  setMockConnectFailure(ERR_DEVICE_NOT_CONNECTED);
  unsigned long ch = 0;

  CHECK(shim().Connect(device, ISO15765, PHYSICAL_CHANNEL | CAN_29BIT_ID,
                       125000, &ch) == ERR_DEVICE_NOT_CONNECTED);
  CHECK(logCount("Connect device=") == 1);
  CHECK_FALSE(logHas("protocol=0x00008005"));

  setMockConnectFailure(STATUS_NOERROR);
  shim().Close(device);
}

TEST_CASE("a strict J2534 adapter is not changed by Mongoose fallbacks") {
  MockModeScope mode(1);
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);

  CHECK(shim().Ioctl(ch, CLEAR_RX_BUFFER, nullptr, nullptr) == STATUS_NOERROR);
  CHECK(logCount("Ioctl channel=") == 1);

  PASSTHRU_MSG rx = {};
  unsigned long num = 1;
  CHECK(shim().ReadMsgs(ch, &rx, &num, 20) == ERR_TIMEOUT);
  CHECK(num == 0);

  PASSTHRU_MSG tx = makeMsg(
      ISO15765, ISO15765_FRAME_PAD | ISO15765_ADDR_TYPE,
      {0x00, 0x00, 0x07, 0xdf, 0x3e, 0x80});
  num = 1;
  CHECK(shim().WriteMsgs(ch, &tx, &num, 100) == STATUS_NOERROR);
  CHECK(num == 1);

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("the MS channel request is translated to the pin-switched route") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;

  // Exactly what SDA asks for.
  REQUIRE(shim().Connect(device, ISO15765, PHYSICAL_CHANNEL | CAN_29BIT_ID, 125000,
                         &ch) == STATUS_NOERROR);

  CHECK_MESSAGE(ch != 0xDEAD, "the id from the refused attempt must not escape");
  CHECK(logHas("protocol=0x00000006")); // the direct attempt, refused
  CHECK(logHas("protocol=0x00008005")); // ISO15765_PS, accepted
  CHECK(logHas("param=0x00008001 value=779")); // J1962_PINS = 0x030B, pins 3/11
  CHECK(logHas("param=0x00000003 value=0")); // LOOPBACK disabled

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("translated MS channels do not feed transmit indications into RX") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, PHYSICAL_CHANNEL | CAN_29BIT_ID, 125000,
                         &ch) == STATUS_NOERROR);

  PASSTHRU_MSG tx = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                            {0x00, 0x00, 0x07, 0x36, 0x22, 0xd1, 0x00});
  unsigned long num = 1;
  REQUIRE(shim().WriteMsgs(ch, &tx, &num, 100) == STATUS_NOERROR);

  PASSTHRU_MSG rx = {};
  num = 1;
  CHECK_MESSAGE(shim().ReadMsgs(ch, &rx, &num, 0) == ERR_BUFFER_EMPTY,
                "LOOPBACK=0 must prevent Mongoose TX indications from poisoning "
                "SDA's receive queue");
  CHECK(num == 0);

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("an empty blocking read is reported as a timeout") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);

  PASSTHRU_MSG rx = {};
  unsigned long num = 1;
  CHECK(shim().ReadMsgs(ch, &rx, &num, 20) == ERR_TIMEOUT);
  CHECK(num == 0);

  num = 1;
  CHECK_MESSAGE(shim().ReadMsgs(ch, &rx, &num, 0) == ERR_BUFFER_EMPTY,
                "non-blocking queue probes must retain ERR_BUFFER_EMPTY");
  CHECK(num == 0);

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("periodic TX indications cannot starve a blocking ECU read") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 125000, &ch) ==
          STATUS_NOERROR);

  PASSTHRU_MSG periodic = makeMsg(
      ISO15765, ISO15765_FRAME_PAD | ISO15765_ADDR_TYPE,
      {0x00, 0x00, 0x07, 0xdf, 0x3e, 0x80});
  unsigned long periodicId = 0;
  REQUIRE(shim().StartPeriodicMsg(ch, &periodic, &periodicId, 2000) ==
          STATUS_NOERROR);

  PASSTHRU_MSG rx = {};
  unsigned long num = 1;
  CHECK_MESSAGE(shim().ReadMsgs(ch, &rx, &num, 100) == ERR_TIMEOUT,
                "repeated 0x7DF TX indications must not reset the host timeout");
  CHECK(num == 0);
  CHECK_MESSAGE(logCount("ReadMsgs channel=") > 1,
                "the mock must reproduce repeated periodic indications");
  CHECK_MESSAGE(readShimLog().find("suppressing TX indication") != std::string::npos,
                "the first periodic suppression must be visible at debug level");

  num = 1;
  CHECK_MESSAGE(shim().ReadMsgs(ch, &rx, &num, 0) == ERR_BUFFER_EMPTY,
                "a non-blocking read containing only a periodic indication "
                "must look empty to SDA");
  CHECK(num == 0);

  REQUIRE(shim().StopPeriodicMsg(ch, periodicId) == STATUS_NOERROR);
  PASSTHRU_MSG request = makeMsg(
      ISO15765, ISO15765_FRAME_PAD,
      {0x00, 0x00, 0x07, 0x36, 0x22, 0xf1, 0x8c});
  num = 1;
  REQUIRE(shim().WriteMsgs(ch, &request, &num, 100) == STATUS_NOERROR);
  num = 1;
  REQUIRE(shim().ReadMsgs(ch, &rx, &num, 100) == STATUS_NOERROR);
  CHECK((rx.RxStatus & TX_MSG_TYPE) != 0);
  CHECK(rx.Data[3] == 0x36);

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("a functional DiCE channel preserves normal ISO15765 addressing") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765,
                         CAN_29BIT_ID | j2534::dice::FUNC_ADDR, 500000, &ch) ==
          STATUS_NOERROR);

  PASSTHRU_MSG tx = makeMsg(
      ISO15765, ISO15765_FRAME_PAD,
      {0x00, 0x00, 0x07, 0xdf, 0x10, 0x82});
  unsigned long num = 1;
  REQUIRE(shim().WriteMsgs(ch, &tx, &num, 100) == STATUS_NOERROR);
  CHECK_MESSAGE(num == 1, "best-effort functional broadcast is reported submitted");
  CHECK(logHas("txflags=0x00000040"));
  CHECK_FALSE(logHas("txflags=0x000000C0"));
  CHECK_FALSE(logHas("txflags=0x10000040"));

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("a functional channel does not mark physical requests functional") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765,
                         CAN_29BIT_ID | j2534::dice::FUNC_ADDR, 500000, &ch) ==
          STATUS_NOERROR);

  PASSTHRU_MSG tx = makeMsg(
      ISO15765, ISO15765_FRAME_PAD,
      {0x00, 0x00, 0x07, 0x36, 0x22, 0xf1, 0x91});
  unsigned long num = 1;
  REQUIRE(shim().WriteMsgs(ch, &tx, &num, 100) == STATUS_NOERROR);
  CHECK_MESSAGE(logHas("txflags=0x00000040"),
                "physical 0x736 must not receive ISO15765_ADDR_TYPE");
  CHECK_FALSE(logHas("txflags=0x000000C0"));

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("an unacknowledged physical write remains an error") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);

  PASSTHRU_MSG tx = makeMsg(
      ISO15765, ISO15765_FRAME_PAD,
      {0x00, 0x00, 0x07, 0xde, 0x22, 0xf1, 0x91});
  unsigned long num = 1;
  CHECK(shim().WriteMsgs(ch, &tx, &num, 100) == ERR_TIMEOUT);
  CHECK(num == 0);

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("messages on a translated channel carry the pin-switched protocol") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, PHYSICAL_CHANNEL | CAN_29BIT_ID, 125000,
                         &ch) == STATUS_NOERROR);

  PASSTHRU_MSG tx = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                            {0x00, 0x00, 0x07, 0x36, 0x22, 0xd1, 0x00});
  const unsigned long originalProtocol = tx.ProtocolID;
  unsigned long num = 1;
  CHECK(shim().WriteMsgs(ch, &tx, &num, 100) == STATUS_NOERROR);

  CHECK(logHas("WriteMsgs channel="));
  CHECK_MESSAGE(logHas("protocol=0x00008005"), "the target must see ISO15765_PS");
  CHECK_MESSAGE(tx.ProtocolID == originalProtocol,
                "SDA only knows ISO15765; its buffer must come back unchanged");

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("replies on a translated channel are handed back as ISO15765") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, PHYSICAL_CHANNEL | CAN_29BIT_ID, 125000,
                         &ch) == STATUS_NOERROR);

  // The driver answers with the protocol it was opened as; SDA would reject it.
  pushRx(ISO15765_PS, {0x00, 0x00, 0x07, 0x3e, 0x62, 0xd1, 0x00, 0x02});

  // One message against a request for four is a short read, which v04.04 calls
  // ERR_TIMEOUT - the normal outcome of every polling read, and the one the
  // shim once treated as "nothing arrived", dropping the translation with it.
  PASSTHRU_MSG rx[4] = {};
  unsigned long num = 4;
  CHECK(shim().ReadMsgs(ch, rx, &num, 100) == ERR_TIMEOUT);
  REQUIRE(num == 1);
  CHECK_MESSAGE(rx[0].ProtocolID == static_cast<unsigned long>(ISO15765),
                "the shim must translate the protocol id back for the host");
  CHECK(rx[0].Data[4] == 0x62);

  // The same message asked for exactly, so the driver has nothing left over.
  pushRx(ISO15765_PS, {0x00, 0x00, 0x07, 0x3e, 0x7e, 0x00});
  PASSTHRU_MSG one[1] = {};
  num = 1;
  CHECK(shim().ReadMsgs(ch, one, &num, 100) == STATUS_NOERROR);
  REQUIRE(num == 1);
  CHECK(one[0].ProtocolID == static_cast<unsigned long>(ISO15765));

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("a filter on a translated channel is rewritten in both directions") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, PHYSICAL_CHANNEL | CAN_29BIT_ID, 125000,
                         &ch) == STATUS_NOERROR);

  PASSTHRU_MSG mask = makeMsg(ISO15765, ISO15765_FRAME_PAD, {0xff, 0xff, 0xff, 0xff});
  PASSTHRU_MSG pattern =
      makeMsg(ISO15765, ISO15765_FRAME_PAD, {0x00, 0x00, 0x07, 0x3e});
  PASSTHRU_MSG flow = makeMsg(ISO15765, ISO15765_FRAME_PAD, {0x00, 0x00, 0x07, 0x36});
  unsigned long filterId = 0;
  CHECK(shim().StartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pattern, &flow,
                              &filterId) == STATUS_NOERROR);

  CHECK_MESSAGE(mask.ProtocolID == static_cast<unsigned long>(ISO15765),
                "mask restored");
  CHECK_MESSAGE(pattern.ProtocolID == static_cast<unsigned long>(ISO15765),
                "pattern restored");
  CHECK_MESSAGE(flow.ProtocolID == static_cast<unsigned long>(ISO15765),
                "flow control restored");

  shim().StopMsgFilter(ch, filterId);
  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("SDA filter stop is redirected to its unique channel owner") {
  const unsigned long device = freshDevice();
  unsigned long wrongChannel = 0;
  unsigned long ownerChannel = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 125000,
                         &wrongChannel) == STATUS_NOERROR);
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000,
                         &ownerChannel) == STATUS_NOERROR);

  PASSTHRU_MSG mask = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                              {0xff, 0xff, 0xff, 0xff});
  PASSTHRU_MSG pattern = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                 {0x00, 0x00, 0x07, 0x9b});
  PASSTHRU_MSG flow = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                              {0x00, 0x00, 0x07, 0x93});
  unsigned long filterId = 0;
  REQUIRE(shim().StartMsgFilter(ownerChannel, FLOW_CONTROL_FILTER, &mask,
                                &pattern, &flow, &filterId) == STATUS_NOERROR);
  REQUIRE(shim().StopMsgFilter(wrongChannel, filterId) == STATUS_NOERROR);
  CHECK(logHas("StopMsgFilter channel=" + std::to_string(ownerChannel) +
               " msg=" + std::to_string(filterId)));

  shim().Disconnect(ownerChannel);
  shim().Disconnect(wrongChannel);
  shim().Close(device);
}

TEST_CASE("ambiguous cross-channel filter ownership is never guessed") {
  const unsigned long device = freshDevice();
  unsigned long first = 0, second = 0, wrong = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &first) ==
          STATUS_NOERROR);
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &second) ==
          STATUS_NOERROR);
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &wrong) ==
          STATUS_NOERROR);

  PASSTHRU_MSG mask = makeMsg(ISO15765, 0, {0xff, 0xff, 0xff, 0xff});
  PASSTHRU_MSG pattern = makeMsg(ISO15765, 0, {0x00, 0x00, 0x07, 0x9b});
  PASSTHRU_MSG flow = makeMsg(ISO15765, 0, {0x00, 0x00, 0x07, 0x93});
  unsigned long firstId = 0, secondId = 0;
  REQUIRE(shim().StartMsgFilter(first, FLOW_CONTROL_FILTER, &mask, &pattern,
                                &flow, &firstId) == STATUS_NOERROR);
  REQUIRE(shim().StartMsgFilter(second, FLOW_CONTROL_FILTER, &mask, &pattern,
                                &flow, &secondId) == STATUS_NOERROR);
  REQUIRE(firstId == secondId);
  CHECK(shim().StopMsgFilter(wrong, firstId) == ERR_INVALID_FILTER_ID);
  CHECK(logCount("StopMsgFilter channel=") == 0);
  CHECK(shim().StopMsgFilter(first, firstId) == STATUS_NOERROR);
  CHECK(logCount("StopMsgFilter channel=") == 1);
  CHECK(shim().StopMsgFilter(second, secondId) == STATUS_NOERROR);

  shim().Disconnect(wrong);
  shim().Disconnect(second);
  shim().Disconnect(first);
  shim().Close(device);
}

TEST_CASE("ordinary TX indication survives periodic on the same CAN id") {
  const unsigned long device = freshDevice();
  unsigned long channel = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &channel) == STATUS_NOERROR);
  PASSTHRU_MSG periodic = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                  {0, 0, 7, 0x36, 0x3e, 0x80});
  unsigned long periodicId = 0;
  REQUIRE(shim().StartPeriodicMsg(channel, &periodic, &periodicId, 2000) == STATUS_NOERROR);
  PASSTHRU_MSG request = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                 {0, 0, 7, 0x36, 0x22, 0xf1, 0x8c});
  unsigned long count = 1;
  REQUIRE(shim().WriteMsgs(channel, &request, &count, 100) == STATUS_NOERROR);
  PASSTHRU_MSG rx = {};
  count = 1;
  CHECK(shim().ReadMsgs(channel, &rx, &count, 100) == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK((rx.RxStatus & TX_MSG_TYPE) != 0);
  count = 1;
  CHECK(shim().ReadMsgs(channel, &rx, &count, 20) == ERR_TIMEOUT);
  CHECK(count == 0);
  shim().StopPeriodicMsg(channel, periodicId);
  shim().Disconnect(channel);
  shim().Close(device);
}

TEST_CASE("full periodic echo cannot consume an ordinary TX reservation") {
  const unsigned long device = freshDevice();
  unsigned long channel = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &channel) == STATUS_NOERROR);
  PASSTHRU_MSG periodic = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                  {0, 0, 7, 0x36, 0x3e, 0x80});
  unsigned long periodicId = 0;
  REQUIRE(shim().StartPeriodicMsg(channel, &periodic, &periodicId, 2000) == STATUS_NOERROR);
  using PushMessage = long(WINAPI *)(const PASSTHRU_MSG *);
  const auto pushMessage = mockFn<PushMessage>("MockPushMessage");
  PASSTHRU_MSG echo = periodic;
  echo.RxStatus = TX_MSG_TYPE | 0x08;
  REQUIRE(pushMessage(&echo) == STATUS_NOERROR);
  PASSTHRU_MSG request = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                 {0, 0, 7, 0x36, 0x22, 0xf1, 0x8c});
  unsigned long count = 1;
  REQUIRE(shim().WriteMsgs(channel, &request, &count, 100) == STATUS_NOERROR);
  PASSTHRU_MSG rx = {};
  count = 1;
  CHECK(shim().ReadMsgs(channel, &rx, &count, 30) == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK(rx.DataSize == 4);
  CHECK((rx.RxStatus & TX_MSG_TYPE) != 0);
  count = 1;
  CHECK(shim().ReadMsgs(channel, &rx, &count, 20) == ERR_TIMEOUT);
  CHECK(count == 0);
  shim().StopPeriodicMsg(channel, periodicId);
  shim().Disconnect(channel);
  shim().Close(device);
}

TEST_CASE("failed write cancels its ordinary TX reservation") {
  const unsigned long device = freshDevice();
  unsigned long channel = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &channel) == STATUS_NOERROR);
  PASSTHRU_MSG periodic = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                  {0, 0, 7, 0xde, 0x3e, 0x80});
  unsigned long periodicId = 0;
  REQUIRE(shim().StartPeriodicMsg(channel, &periodic, &periodicId, 2000) == STATUS_NOERROR);
  PASSTHRU_MSG request = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                 {0, 0, 7, 0xde, 0x22, 0xf1, 0x8c});
  unsigned long count = 1;
  REQUIRE(shim().WriteMsgs(channel, &request, &count, 100) == ERR_TIMEOUT);
  CHECK(count == 0);
  PASSTHRU_MSG rx = {};
  count = 1;
  CHECK(shim().ReadMsgs(channel, &rx, &count, 20) == ERR_TIMEOUT);
  CHECK(count == 0);
  shim().StopPeriodicMsg(channel, periodicId);
  shim().Disconnect(channel);
  shim().Close(device);
}

TEST_CASE("TX reservation exists before the vendor write returns") {
  const unsigned long device = freshDevice();
  unsigned long channel = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &channel) == STATUS_NOERROR);
  PASSTHRU_MSG periodic = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                  {0, 0, 7, 0x36, 0x3e, 0x80});
  unsigned long periodicId = 0;
  REQUIRE(shim().StartPeriodicMsg(channel, &periodic, &periodicId, 2000) == STATUS_NOERROR);
  using SetDelay = long(WINAPI *)(unsigned long);
  const auto setDelay = mockFn<SetDelay>("MockSetWriteReturnDelay");
  REQUIRE(setDelay(150) == STATUS_NOERROR);
  PASSTHRU_MSG request = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                                 {0, 0, 7, 0x36, 0x22, 0xf1, 0x8c});
  std::atomic<bool> returned{false};
  long writeStatus = ERR_FAILED;
  std::thread writer([&] {
    unsigned long count = 1;
    writeStatus = shim().WriteMsgs(channel, &request, &count, 500);
    returned.store(true);
  });
  for (int i = 0; i < 100 && !logHas("WriteMsgs channel="); ++i)
    Sleep(1);
  PASSTHRU_MSG rx = {};
  unsigned long count = 1;
  const long readStatus = shim().ReadMsgs(channel, &rx, &count, 30);
  const bool readBeforeReturn = !returned.load();
  writer.join();
  setDelay(0);
  CHECK(writeStatus == STATUS_NOERROR);
  CHECK(readBeforeReturn);
  CHECK(readStatus == STATUS_NOERROR);
  CHECK(count == 1);
  CHECK((rx.RxStatus & TX_MSG_TYPE) != 0);
  shim().StopPeriodicMsg(channel, periodicId);
  shim().Disconnect(channel);
  shim().Close(device);
}

TEST_CASE("CLEAR_MSG_FILTERS removes filter ownership state") {
  const unsigned long device = freshDevice();
  unsigned long wrongChannel = 0, ownerChannel = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 125000,
                         &wrongChannel) == STATUS_NOERROR);
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000,
                         &ownerChannel) == STATUS_NOERROR);
  PASSTHRU_MSG mask = makeMsg(ISO15765, 0, {0xff, 0xff, 0xff, 0xff});
  PASSTHRU_MSG pattern = makeMsg(ISO15765, 0, {0x00, 0x00, 0x07, 0x9b});
  PASSTHRU_MSG flow = makeMsg(ISO15765, 0, {0x00, 0x00, 0x07, 0x93});
  unsigned long filterId = 0;
  REQUIRE(shim().StartMsgFilter(ownerChannel, FLOW_CONTROL_FILTER, &mask,
                                &pattern, &flow, &filterId) == STATUS_NOERROR);
  REQUIRE(shim().Ioctl(ownerChannel, CLEAR_MSG_FILTERS, nullptr, nullptr) ==
          STATUS_NOERROR);
  CHECK(shim().StopMsgFilter(wrongChannel, filterId) == ERR_INVALID_FILTER_ID);
  CHECK(logHas("StopMsgFilter channel=" + std::to_string(wrongChannel)));

  shim().Disconnect(ownerChannel);
  shim().Disconnect(wrongChannel);
  shim().Close(device);
}

TEST_CASE("channel state does not survive PassThruClose") {
  // SDA closes the device without disconnecting its channels on every error
  // path, and drivers reissue the same small channel ids afterwards. A
  // left-over translation entry would make the shim rewrite ProtocolID on a
  // channel that was never translated.
  unsigned long device = freshDevice();
  unsigned long msCh = 0;
  REQUIRE(shim().Connect(device, ISO15765, PHYSICAL_CHANNEL | CAN_29BIT_ID, 125000,
                         &msCh) == STATUS_NOERROR);

  // Close without disconnecting, exactly as SDA does.
  REQUIRE(shim().Close(device) == STATUS_NOERROR);

  device = freshDevice();
  unsigned long hsCh = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &hsCh) ==
          STATUS_NOERROR);
  CHECK_MESSAGE(hsCh == msCh, "the mock must reissue the id for this test to mean "
                              "anything");

  resetMock();
  PASSTHRU_MSG tx = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                            {0x00, 0x00, 0x07, 0xe0, 0x22, 0xf1, 0xa3});
  unsigned long num = 1;
  CHECK(shim().WriteMsgs(hsCh, &tx, &num, 100) == STATUS_NOERROR);
  CHECK_MESSAGE(!logHas("protocol=0x00008005"),
                "a plain HS channel must not inherit the MS translation");
  CHECK(logHas("protocol=0x00000006"));

  shim().Disconnect(hsCh);
  shim().Close(device);
}

TEST_CASE("closing one device preserves translated channels of another") {
  unsigned long deviceA = 0, deviceB = 0;
  REQUIRE(shim().Open(nullptr, &deviceA) == STATUS_NOERROR);
  REQUIRE(shim().Open(nullptr, &deviceB) == STATUS_NOERROR);
  resetMock();

  unsigned long chA = 0, chB = 0;
  REQUIRE(shim().Connect(deviceA, ISO15765, CAN_29BIT_ID, 500000, &chA) ==
          STATUS_NOERROR);
  REQUIRE(shim().Connect(deviceB, ISO15765, PHYSICAL_CHANNEL | CAN_29BIT_ID,
                         125000, &chB) == STATUS_NOERROR);
  REQUIRE(shim().Close(deviceA) == STATUS_NOERROR);

  resetMock();
  PASSTHRU_MSG tx = makeMsg(ISO15765, ISO15765_FRAME_PAD,
                            {0x00, 0x00, 0x07, 0x36, 0x22, 0xd1, 0x00});
  unsigned long num = 1;
  CHECK(shim().WriteMsgs(chB, &tx, &num, 100) == STATUS_NOERROR);
  CHECK_MESSAGE(logHas("protocol=0x00008005"),
                "closing another device must not erase MS protocol translation");

  shim().Disconnect(chB);
  shim().Close(deviceB);
}

TEST_CASE("a failed Connect reports no channel id") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0xAAAA;
  // The mock refuses PHYSICAL_CHANNEL and there is no pin-switched form of a
  // protocol it does not know, so the fallback cannot rescue this one.
  const long r = shim().Connect(device, ISO9141, PHYSICAL_CHANNEL, 10400, &ch);
  CHECK(r != STATUS_NOERROR);
  CHECK_MESSAGE(ch == 0, "a failed Connect must not leave a usable-looking id");
  shim().Close(device);
}

// --- CLEAR_RX_BUFFER ---------------------------------------------------------

TEST_CASE("CLEAR_RX_BUFFER is emulated when the target refuses it") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);

  // Three messages waiting, exactly the situation SDA is trying to clear: its
  // own transmit indications from the previous request.
  pushRx(ISO15765, {0x00, 0x00, 0x07, 0xe8, 0x01});
  pushRx(ISO15765, {0x00, 0x00, 0x07, 0xe8, 0x02});
  pushRx(ISO15765, {0x00, 0x00, 0x07, 0xe8, 0x03});

  CHECK_MESSAGE(shim().Ioctl(ch, CLEAR_RX_BUFFER, nullptr, nullptr) ==
                    STATUS_NOERROR,
                "the mock refuses this the way MongoosePro JLR does; the shim "
                "must establish the postcondition itself");
  CHECK_MESSAGE(logCount("ReadMsgs channel=") == 2,
                "a short drain read must be followed by an explicit empty "
                "probe");

  PASSTHRU_MSG rx[4] = {};
  unsigned long num = 4;
  CHECK_MESSAGE(shim().ReadMsgs(ch, rx, &num, 0) == ERR_BUFFER_EMPTY,
                "the queue must actually be empty, not merely reported so");
  CHECK(num == 0);

  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("CLEAR_RX_BUFFER on an empty queue still succeeds") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);
  CHECK(shim().Ioctl(ch, CLEAR_RX_BUFFER, nullptr, nullptr) == STATUS_NOERROR);
  shim().Disconnect(ch);
  shim().Close(device);
}

TEST_CASE("CLEAR_RX_BUFFER emulation is announced once per channel") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);
  const size_t logSizeBefore = readShimLog().size();

  // SDA issues this before every request. A live flash read produced 2150 of
  // them and drowned the log; only the first may be announced.
  for (int i = 0; i < 20; ++i)
    REQUIRE(shim().Ioctl(ch, CLEAR_RX_BUFFER, nullptr, nullptr) == STATUS_NOERROR);

  const std::string tail = readShimLog().substr(logSizeBefore);
  size_t announcements = 0;
  for (size_t at = tail.find("emulating by draining"); at != std::string::npos;
       at = tail.find("emulating by draining", at + 1))
    ++announcements;
  INFO("shim log tail:\n" << tail);
  CHECK_MESSAGE(announcements == 1,
                "twenty emulated calls must produce one line at info level");

  shim().Disconnect(ch);
  shim().Close(device);
}

// --- ISO15765_SWDL_FABRICATE instrumentation ---------------------------------

TEST_CASE("an oversized write in fabricate mode fails closed") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765, CAN_29BIT_ID, 500000, &ch) ==
          STATUS_NOERROR);
  REQUIRE(setConfig(ch, {{j2534::dice::ISO15765_SWDL_FABRICATE, 1}}) ==
          STATUS_NOERROR);
  const size_t logSizeBefore = readShimLog().size();

  // Larger than one ISO-TP message: the shim cannot split this into
  // TransferData (0x36) frames, so it must not forward it in a malformed form.
  PASSTHRU_MSG big = makeMsg(ISO15765, CAN_29BIT_ID, {0x00, 0x00, 0x07, 0xe0, 0x36});
  big.DataSize = 5000;
  unsigned long num = 1;
  CHECK(shim().WriteMsgs(ch, &big, &num, 100) == ERR_NOT_SUPPORTED);
  CHECK(num == 0);

  const std::string tail = readShimLog().substr(logSizeBefore);
  INFO("shim log tail:\n" << tail);
  CHECK_MESSAGE(tail.find("FABRICATE:") != std::string::npos,
                "the shim must report a write it cannot split into 0x36 frames");

  CHECK_FALSE(logHas("size=5000"));

  // The decisive case, and the one a live SDA session actually exercises: SDA
  // sets fabricate mode once, never clears it, and keeps doing ordinary UDS on
  // the same channel. Blocking those writes broke plain DID reads and wake
  // commands, so a normally sized write must still reach the target while the
  // mode is on.
  num = 1;
  PASSTHRU_MSG small =
      makeMsg(ISO15765, CAN_29BIT_ID, {0x00, 0x00, 0x07, 0xe0, 0x22, 0xf1, 0xa3});
  CHECK(shim().WriteMsgs(ch, &small, &num, 100) == STATUS_NOERROR);
  CHECK(num == 1);
  CHECK(logHas("22 f1 a3"));

  shim().Disconnect(ch);
  shim().Close(device);
}

// --- the acceptance criterion from the specification --------------------------

TEST_CASE("no vendor id ever reaches the target driver") {
  const unsigned long device = freshDevice();
  unsigned long ch = 0;
  REQUIRE(shim().Connect(device, ISO15765,
                         CAN_29BIT_ID | j2534::dice::FUNC_ADDR, 500000,
                         &ch) == STATUS_NOERROR);

  setConfig(ch, {{j2534::dice::ISO15765_SWDL_FABRICATE, 1}});
  setConfig(ch, {{j2534::dice::CAN_XON_XOFF_STMIN, 0}});
  shim().Ioctl(ch, j2534::dice::DICE_CAN_XON_XOFF_FILTER, nullptr, nullptr);
  shim().Ioctl(ch, CAN_XON_XOFF_FILTER_ACTIVE, nullptr, nullptr);

  unsigned long badCh = 0;
  shim().Connect(device, j2534::dice::CAN_XON_XOFF_STMIN, 0, 125000, &badCh);

  PASSTHRU_MSG tx = makeMsg(ISO15765, 0x20000000UL | j2534::dice::FUNC_ADDR,
                            {0x00, 0x00, 0x07, 0xdf, 0x3e, 0x80});
  unsigned long num = 1;
  shim().WriteMsgs(ch, &tx, &num, 100);

  const std::string log = callLog();
  INFO("mock call log:\n" << log);

  // Vendor ids render as id=0x1..../id=0x2....; standard ones are zero-padded
  // and start id=0x0.
  CHECK(log.find("id=0x1") == std::string::npos);
  CHECK(log.find("id=0x2") == std::string::npos);
  CHECK(log.find("param=0x1") == std::string::npos);
  CHECK(log.find("protocol=0x10000001") == std::string::npos);
  CHECK(log.find("flags=0x10000") == std::string::npos);
  CHECK(log.find("txflags=0x1") == std::string::npos);
  CHECK(log.find("txflags=0x2") == std::string::npos);

  shim().Disconnect(ch);
  shim().Close(device);
}
