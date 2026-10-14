// mockj2534.dll - a fake "real" J2534 driver for testing the shim without any
// hardware. It records every call in an in-memory log that the test host reads
// back through MockGetCallLog, and returns canned answers.
//
// The point of the mock is the safety check from the specification: no vendor
// ioctl id may ever appear in this log.

#include "../shim/include/J2534_v0404.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

namespace {

CRITICAL_SECTION g_cs;
std::string g_log;
unsigned long g_nextChannel = 100;
unsigned long g_nextDevice = 1;
struct ChannelState {
  unsigned long protocol = 0;
  unsigned long baud = 0;
  bool loopback = true;
  std::map<unsigned long, PASSTHRU_MSG> periodic;
  std::set<unsigned long> filters;
  unsigned long nextFilterId = 3;
};
std::map<unsigned long, ChannelState> g_channels;
enum class MockMode : unsigned long { mongoose = 0, strict = 1, nativeDualCan = 2 };
MockMode g_mode = MockMode::mongoose;
long g_connectFailure = STATUS_NOERROR;
volatile LONG g_writeReturnDelay = 0;

// Messages the test has queued for PassThruReadMsgs to hand back, so the
// receive direction of the shim's protocol translation can be observed.
std::vector<PASSTHRU_MSG> g_rx;

void ensureCs();

void queueTxIndication(unsigned long channel, const PASSTHRU_MSG &tx) {
  ensureCs();
  EnterCriticalSection(&g_cs);
  const auto it = g_channels.find(channel);
  if (it != g_channels.end() && it->second.loopback) {
    PASSTHRU_MSG indication = tx;
    indication.RxStatus = TX_MSG_TYPE | 0x00000008UL; // DiCE TX_INDICATION
    indication.TxFlags = 0;
    indication.DataSize = tx.DataSize >= 4 ? 4 : tx.DataSize;
    g_rx.push_back(indication);
  }
  LeaveCriticalSection(&g_cs);
}

void ensureCs() {
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(
      &once,
      [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        InitializeCriticalSection(&g_cs);
        return TRUE;
      },
      nullptr, nullptr);
}

void record(const char *fmt, ...) {
  ensureCs();
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);

  EnterCriticalSection(&g_cs);
  g_log += buf;
  g_log += "\n";
  LeaveCriticalSection(&g_cs);
}

std::string hex(const unsigned char *d, unsigned long n) {
  static const char *k = "0123456789abcdef";
  std::string s;
  for (unsigned long i = 0; i < n; ++i) {
    if (i)
      s += ' ';
    s += k[d[i] >> 4];
    s += k[d[i] & 0xf];
  }
  return s;
}

} // namespace

extern "C" {

long WINAPI PassThruOpen(void *pName, unsigned long *pDeviceID) {
  record("Open name=%s", pName ? static_cast<const char *>(pName) : "");
  if (pDeviceID)
    *pDeviceID = g_nextDevice++;
  return STATUS_NOERROR;
}

long WINAPI PassThruClose(unsigned long DeviceID) {
  record("Close device=%lu", DeviceID);
  return STATUS_NOERROR;
}

long WINAPI PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                            unsigned long Flags, unsigned long Baudrate,
                            unsigned long *pChannelID) {
  record("Connect device=%lu protocol=0x%08lX flags=0x%08lX baud=%lu", DeviceID,
         ProtocolID, Flags, Baudrate);
  if (g_connectFailure != STATUS_NOERROR)
    return g_connectFailure;
  if (ProtocolID != ISO15765 && ProtocolID != CAN && ProtocolID != CAN_PS &&
      ProtocolID != ISO15765_PS)
    return ERR_INVALID_PROTOCOL_ID;

  // Behave like MongoosePro JLR: no second physical CAN, so PHYSICAL_CHANNEL is
  // refused and the MS bus is only reachable through the pin-switched
  // protocols. Without this the shim's MS fallback would never run under test,
  // which is how it stayed uncovered while shipping to live hardware.
  if ((Flags & PHYSICAL_CHANNEL) && g_mode != MockMode::nativeDualCan) {
    // Deliberately writes a plausible-looking id on the failure path, so the
    // shim is held to not leaking it back to the host.
    if (pChannelID)
      *pChannelID = 0xDEAD;
    return ERR_INVALID_FLAGS;
  }

  if (pChannelID) {
    *pChannelID = g_nextChannel++;
    ensureCs();
    EnterCriticalSection(&g_cs);
    g_channels[*pChannelID] =
        {ProtocolID, Baudrate, g_mode == MockMode::mongoose};
    LeaveCriticalSection(&g_cs);
  }
  return STATUS_NOERROR;
}

long WINAPI PassThruDisconnect(unsigned long ChannelID) {
  record("Disconnect channel=%lu", ChannelID);
  ensureCs();
  EnterCriticalSection(&g_cs);
  g_channels.erase(ChannelID);
  LeaveCriticalSection(&g_cs);
  return STATUS_NOERROR;
}

long WINAPI PassThruReadMsgs(unsigned long ChannelID, void *pMsg,
                             unsigned long *pNumMsgs, unsigned long Timeout) {
  record("ReadMsgs channel=%lu num=%lu timeout=%lu", ChannelID,
         pNumMsgs ? *pNumMsgs : 0UL, Timeout);
  ensureCs();
  if (Timeout) {
    EnterCriticalSection(&g_cs);
    const bool queueEmpty = g_rx.empty();
    LeaveCriticalSection(&g_cs);
    if (queueEmpty)
      Sleep(1);
  }
  EnterCriticalSection(&g_cs);
  const auto channel = g_channels.find(ChannelID);
  if (g_rx.empty() && channel != g_channels.end() &&
      !channel->second.periodic.empty()) {
    PASSTHRU_MSG indication = channel->second.periodic.begin()->second;
    indication.RxStatus = TX_MSG_TYPE | 0x00000008UL;
    indication.TxFlags = 0;
    indication.DataSize = indication.DataSize >= 4 ? 4 : indication.DataSize;
    g_rx.push_back(indication);
  }
  const unsigned long capacity = pNumMsgs ? *pNumMsgs : 0UL;
  unsigned long delivered = 0;
  const bool receiveEnabled =
      channel == g_channels.end() || channel->second.protocol == ISO15765 ||
      channel->second.protocol == ISO15765_PS || !channel->second.filters.empty();
  while (receiveEnabled && delivered < capacity && !g_rx.empty() && pMsg) {
    static_cast<PASSTHRU_MSG *>(pMsg)[delivered++] = g_rx.front();
    g_rx.erase(g_rx.begin());
  }
  LeaveCriticalSection(&g_cs);

  if (pNumMsgs)
    *pNumMsgs = delivered;

  // What a real driver does, and what v04.04 specifies: a short read is
  // ERR_TIMEOUT with the messages that did arrive, not an empty failure. The
  // shim has to keep translating those.
  if (delivered == 0)
    return Timeout && g_mode != MockMode::mongoose ? ERR_TIMEOUT
                                                   : ERR_BUFFER_EMPTY;
  return delivered < capacity ? ERR_TIMEOUT : STATUS_NOERROR;
}

long WINAPI PassThruWriteMsgs(unsigned long ChannelID, void *pMsg,
                              unsigned long *pNumMsgs, unsigned long /*Timeout*/) {
  const unsigned long n = pNumMsgs ? *pNumMsgs : 0UL;
  bool unacknowledgedBroadcast = false;
  for (unsigned long i = 0; i < n && pMsg; ++i) {
    const PASSTHRU_MSG *m = static_cast<const PASSTHRU_MSG *>(pMsg) + i;
    unsigned long size = m->DataSize;
    if (size > sizeof(m->Data))
      size = sizeof(m->Data);
    record("WriteMsgs channel=%lu protocol=0x%08lX txflags=0x%08lX size=%lu data=%s",
           ChannelID, m->ProtocolID, m->TxFlags, m->DataSize,
           hex(m->Data, size).c_str());
    ensureCs();
    EnterCriticalSection(&g_cs);
    const auto channel = g_channels.find(ChannelID);
    const bool hs = channel != g_channels.end() && channel->second.baud == 500000;
    LeaveCriticalSection(&g_cs);
    const bool noAckTarget =
        m->DataSize >= 4 && m->Data[0] == 0 && m->Data[1] == 0 &&
        m->Data[2] == 0x07 && (m->Data[3] == 0xDF || m->Data[3] == 0xDE);
    if (hs && noAckTarget && g_mode == MockMode::mongoose)
      unacknowledgedBroadcast = true;
    else
      queueTxIndication(ChannelID, *m);
  }
  const LONG writeDelay = InterlockedCompareExchange(&g_writeReturnDelay, 0, 0);
  if (writeDelay > 0)
    Sleep(static_cast<DWORD>(writeDelay));
  if (unacknowledgedBroadcast) {
    if (pNumMsgs)
      *pNumMsgs = 0;
    return ERR_TIMEOUT;
  }
  return STATUS_NOERROR;
}

long WINAPI PassThruStartPeriodicMsg(unsigned long ChannelID, void *pMsg,
                                     unsigned long *pMsgID, unsigned long Interval) {
  const PASSTHRU_MSG *message = static_cast<const PASSTHRU_MSG *>(pMsg);
  const unsigned long size = message ? std::min<unsigned long>(
                                            message->DataSize,
                                            sizeof(message->Data))
                                     : 0;
  record("StartPeriodicMsg channel=%lu interval=%lu protocol=0x%08lX "
         "txflags=0x%08lX size=%lu data=%s",
         ChannelID, Interval, message ? message->ProtocolID : 0UL,
         message ? message->TxFlags : 0UL, message ? message->DataSize : 0UL,
         message ? hex(message->Data, size).c_str() : "");
  const unsigned long id = 7;
  if (pMsgID)
    *pMsgID = id;
  if (pMsg) {
    ensureCs();
    EnterCriticalSection(&g_cs);
    const auto channel = g_channels.find(ChannelID);
    if (channel != g_channels.end())
      channel->second.periodic[id] = *static_cast<PASSTHRU_MSG *>(pMsg);
    LeaveCriticalSection(&g_cs);
  }
  return STATUS_NOERROR;
}

long WINAPI PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID) {
  record("StopPeriodicMsg channel=%lu msg=%lu", ChannelID, MsgID);
  ensureCs();
  EnterCriticalSection(&g_cs);
  const auto channel = g_channels.find(ChannelID);
  if (channel != g_channels.end())
    channel->second.periodic.erase(MsgID);
  LeaveCriticalSection(&g_cs);
  return STATUS_NOERROR;
}

long WINAPI PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                                   void * /*pMask*/, void * /*pPattern*/, void *pFlow,
                                   unsigned long *pMsgID) {
  record("StartMsgFilter channel=%lu type=%lu flow=%d", ChannelID, FilterType,
         pFlow ? 1 : 0);
  ensureCs();
  EnterCriticalSection(&g_cs);
  const auto channel = g_channels.find(ChannelID);
  unsigned long id = 0;
  if (channel != g_channels.end()) {
    id = channel->second.nextFilterId++;
    channel->second.filters.insert(id);
  }
  LeaveCriticalSection(&g_cs);
  if (pMsgID)
    *pMsgID = id;
  return STATUS_NOERROR;
}

long WINAPI PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID) {
  record("StopMsgFilter channel=%lu msg=%lu", ChannelID, MsgID);
  ensureCs();
  EnterCriticalSection(&g_cs);
  const auto channel = g_channels.find(ChannelID);
  const bool found = channel != g_channels.end() &&
                     channel->second.filters.erase(MsgID) != 0;
  LeaveCriticalSection(&g_cs);
  return found ? STATUS_NOERROR : ERR_INVALID_FILTER_ID;
}

long WINAPI PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin,
                                          unsigned long Voltage) {
  record("SetProgrammingVoltage device=%lu pin=%lu voltage=%lu", DeviceID, Pin,
         Voltage);
  return STATUS_NOERROR;
}

long WINAPI PassThruReadVersion(unsigned long DeviceID, char *pFw, char *pDll,
                                char *pApi) {
  record("ReadVersion device=%lu", DeviceID);
  if (pFw)
    strcpy_s(pFw, 80, "mock-fw-1.0");
  if (pDll)
    strcpy_s(pDll, 80, "mock-dll-1.0");
  if (pApi)
    strcpy_s(pApi, 80, J2534_APIVER_NOVEMBER_2004);
  return STATUS_NOERROR;
}

long WINAPI PassThruGetLastError(char *pErrorDescription) {
  if (pErrorDescription)
    strcpy_s(pErrorDescription, 80, "mock: no error");
  return STATUS_NOERROR;
}

long WINAPI PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, void *pInput,
                          void * /*pOutput*/) {
  record("Ioctl channel=%lu id=0x%08lX", ChannelID, IoctlID);

  // MongoosePro JLR refuses this one, so the mock does too: it is a standard
  // v04.04 ioctl that SDA calls before every request, and the shim has to make
  // up the difference.
  if (IoctlID == CLEAR_RX_BUFFER && g_mode == MockMode::mongoose)
    return ERR_FAILED;

  if (IoctlID == CLEAR_PERIODIC_MSGS || IoctlID == CLEAR_MSG_FILTERS) {
    ensureCs();
    EnterCriticalSection(&g_cs);
    const auto channel = g_channels.find(ChannelID);
    if (channel != g_channels.end()) {
      if (IoctlID == CLEAR_PERIODIC_MSGS)
        channel->second.periodic.clear();
      else
        channel->second.filters.clear();
    }
    LeaveCriticalSection(&g_cs);
  }

  if ((IoctlID == SET_CONFIG || IoctlID == GET_CONFIG) && pInput) {
    SCONFIG_LIST *l = static_cast<SCONFIG_LIST *>(pInput);
    for (uint32_t i = 0; i < l->NumOfParams && l->ConfigPtr; ++i) {
      record("  %s param=0x%08X value=%u",
             IoctlID == SET_CONFIG ? "SET_CONFIG" : "GET_CONFIG",
             l->ConfigPtr[i].Parameter, l->ConfigPtr[i].Value);
      // GET_CONFIG answers with a recognisable value so the shim's copy-back
      // can be verified.
      if (IoctlID == GET_CONFIG)
        l->ConfigPtr[i].Value = 0xC0DE0000u | l->ConfigPtr[i].Parameter;
      if (IoctlID == SET_CONFIG && l->ConfigPtr[i].Parameter == LOOPBACK) {
        ensureCs();
        EnterCriticalSection(&g_cs);
        const auto channel = g_channels.find(ChannelID);
        if (channel != g_channels.end())
          channel->second.loopback = l->ConfigPtr[i].Value != 0;
        LeaveCriticalSection(&g_cs);
      }
    }
  }
  return STATUS_NOERROR;
}

// --- test helpers -----------------------------------------------------------

long WINAPI MockGetCallLog(char *pBuffer, unsigned long *pSize) {
  ensureCs();
  EnterCriticalSection(&g_cs);
  const unsigned long need = static_cast<unsigned long>(g_log.size()) + 1;
  if (!pBuffer || !pSize || *pSize < need) {
    if (pSize)
      *pSize = need;
    LeaveCriticalSection(&g_cs);
    return ERR_BUFFER_FULL;
  }
  memcpy(pBuffer, g_log.c_str(), need);
  *pSize = need;
  LeaveCriticalSection(&g_cs);
  return STATUS_NOERROR;
}

long WINAPI MockReset(void) {
  InterlockedExchange(&g_writeReturnDelay, 0);
  ensureCs();
  EnterCriticalSection(&g_cs);
  g_log.clear();
  g_rx.clear();
  g_nextChannel = 100;
  LeaveCriticalSection(&g_cs);
  return STATUS_NOERROR;
}

// 0=Mongoose quirks, 1=strict v04.04, 2=native second CAN channel.
long WINAPI MockSetWriteReturnDelay(unsigned long milliseconds) {
  InterlockedExchange(&g_writeReturnDelay, static_cast<LONG>(milliseconds));
  return STATUS_NOERROR;
}

long WINAPI MockSetMode(unsigned long mode) {
  if (mode > static_cast<unsigned long>(MockMode::nativeDualCan))
    return ERR_INVALID_IOCTL_ID;
  ensureCs();
  EnterCriticalSection(&g_cs);
  g_mode = static_cast<MockMode>(mode);
  g_connectFailure = STATUS_NOERROR;
  g_rx.clear();
  g_channels.clear();
  LeaveCriticalSection(&g_cs);
  return STATUS_NOERROR;
}

long WINAPI MockSetConnectFailure(long status) {
  ensureCs();
  EnterCriticalSection(&g_cs);
  g_connectFailure = status;
  LeaveCriticalSection(&g_cs);
  return STATUS_NOERROR;
}

// Queues one message for the next PassThruReadMsgs. `pData` is the raw payload
// including the four identifier bytes, exactly as a driver would deliver it.
long WINAPI MockPushRx(unsigned long ProtocolID, const unsigned char *pData,
                       unsigned long Size) {
  if (!pData || Size > sizeof(PASSTHRU_MSG::Data))
    return ERR_NULL_PARAMETER;
  PASSTHRU_MSG m = {};
  m.ProtocolID = ProtocolID;
  m.DataSize = Size;
  memcpy(m.Data, pData, Size);

  ensureCs();
  EnterCriticalSection(&g_cs);
  g_rx.push_back(m);
  LeaveCriticalSection(&g_cs);
  return STATUS_NOERROR;
}

} // extern "C"
