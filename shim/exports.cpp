// A J2534 v04.04 pass-through shim.
//
// SDA 4.7.0.2 loads an arbitrary J2534 DLL by path. This DLL presents the
// standard J2534 API to SDA, forwards standard calls to a real driver
// (Mongoose etc.) and absorbs the DiCE vendor extensions that SDA emits.
//
// Absorbing rather than forwarding is a safety requirement: DiCE and DrewTech
// assign different meanings to the same vendor ids, so a forwarded vendor id
// would invoke a foreign function on the target device. The whitelist covers
// every channel through which a vendor id can travel: ProtocolID, ioctl id,
// SET_CONFIG/GET_CONFIG parameters, PassThruConnect Flags and TxFlags inside
// PASSTHRU_MSG.
//
// Threading: shim state (target entry points, last error, per-channel state)
// is protected by g_lock. The lock is never held across a call into the target
// driver, so a blocking ReadMsgs on one channel cannot stall another thread.
// The Config is written once during initialisation and read-only afterwards.

#include "config.h"
#include "d2/d2_transport.h"
#include "product.h"
#include "include/DiCE.hpp"
#include "include/J2534_v0404.h"
#include "intercept.h"
#include "log.h"
#include "target.h"

#include <map>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using namespace shim;

namespace {

HMODULE g_self = nullptr;
Config g_cfg; // immutable after initOnce
Target g_target;
INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_lock;
volatile LONG g_lockReady = 0;
std::string g_lastError; // empty == defer to the target driver
std::set<unsigned long> g_fabricateChannels;
struct PeriodicMessage {
  unsigned long protocol;
  unsigned long canId;
  std::shared_ptr<class D2PeriodicScheduler> scheduler;
};
// Suppression is keyed by the target protocol and CAN ID, not payload. A
// periodic echo and a normal request to the same ECU endpoint are therefore
// treated as the same target-owned TX indication class by design.
// LOOPBACK=0 is the primary defense and is applied during channel setup;
// this read-side filter is a diagnostic fallback for drivers that ignore it.
std::map<unsigned long, std::map<unsigned long, PeriodicMessage>> g_periodic;
std::set<std::pair<unsigned long, unsigned long>> g_periodicSuppressionLogged;

// RAII lock that tolerates being used before InitializeCriticalSection and
// after DllMain has torn it down.
struct Guard {
  Guard() : held_(InterlockedCompareExchange(&g_lockReady, 1, 1) == 1) {
    if (held_)
      EnterCriticalSection(&g_lock);
  }
  ~Guard() {
    if (held_)
      LeaveCriticalSection(&g_lock);
  }

private:
  bool held_;
};

void setLastError(const std::string &text) {
  Guard g;
  g_lastError = text;
}

// Dropped at the start of every export other than PassThruGetLastError, so the
// message describes the most recent call rather than sticking around forever.
void clearLastError() {
  Guard g;
  g_lastError.clear();
}

BOOL CALLBACK initOnce(PINIT_ONCE, PVOID, PVOID *) {
  // An exception escaping this callback would leave the INIT_ONCE in an
  // undefined state, so nothing is allowed out of here.
  try {
    InitializeCriticalSection(&g_lock);
    InterlockedExchange(&g_lockReady, 1);

    const std::string dir = moduleDirectory(g_self);
    const std::string ini = dir + SHIM_INI_NAME;

    std::string err;
    g_cfg = Config::load(ini, &err);

    std::string logPath = g_cfg.logPath;
    if (logPath.empty())
      logPath = dir + SHIM_PRODUCT_NAME ".log";
    Log::instance().open(logPath, parseLevel(g_cfg.logLevel));

    SHIM_INFO(SHIM_PRODUCT_NAME " " SHIM_VERSION_STR " initialised, ini [%s]%s",
              ini.c_str(), err.empty() ? "" : " (using defaults)");
    if (!err.empty())
      SHIM_ERROR("Config: %s", err.c_str());
    for (const std::string &w : g_cfg.warnings)
      SHIM_ERROR("Config: %s", w.c_str());
    for (const std::string &k : unknownInterceptKeys(g_cfg))
      SHIM_ERROR("Config: [intercept] %s matches no known ioctl, config "
                 "parameter, protocol or flag bit - the policy has no effect",
                 k.c_str());
    if (!g_cfg.d2RouteOverride.empty())
      SHIM_INFO("D2 route overridden by %s=%s; the INI value was not used",
                SHIM_ENV_D2_ROUTE, g_cfg.d2RouteOverride.c_str());
    SHIM_INFO("Target library [%s]", g_cfg.library.c_str());
  } catch (...) {
    // Nothing usable to log to at this point; the exports will report
    // ERR_DEVICE_NOT_CONNECTED because no target could be loaded.
  }
  return TRUE;
}

void ensureInit() { InitOnceExecuteOnce(&g_initOnce, initOnce, nullptr, nullptr); }

// Snapshot of the resolved entry points, taken under the lock so the pointers
// stay valid for the duration of the call without holding the lock.
Target targetSnapshot() {
  Guard g;
  return g_target;
}

long noTarget(const char *fn) {
  SHIM_ERROR("%s called but no target driver is loaded", fn);
  setLastError(SHIM_PRODUCT_NAME ": target J2534 driver not loaded");
  return ERR_DEVICE_NOT_CONNECTED;
}

const char *err2str(long code) { return errorName(code); }

// --- vendor bits inside flag words ------------------------------------------

bool channelFunctional(unsigned long channel);

bool isFunctionalBroadcast(const PASSTHRU_MSG &msg) {
  if ((msg.ProtocolID != ISO15765 && msg.ProtocolID != ISO15765_PS) ||
      msg.DataSize < 4)
    return false;
  const unsigned long id =
      (static_cast<unsigned long>(msg.Data[0]) << 24) |
      (static_cast<unsigned long>(msg.Data[1]) << 16) |
      (static_cast<unsigned long>(msg.Data[2]) << 8) | msg.Data[3];
  return id == 0x7DF;
}

// Temporarily clears vendor bits from the TxFlags of a caller-owned message
// array and restores the originals when it goes out of scope. The host's
// buffer is left exactly as it was found.
class TxFlagScrubber {
public:
  TxFlagScrubber(unsigned long channel, PASSTHRU_MSG *msgs, unsigned long count,
                 const char *fn, bool applyChannelAddressing = true)
      : msgs_(msgs) {
    if (!msgs)
      return;
    const bool functionalChannel =
        applyChannelAddressing && channelFunctional(channel);
    for (unsigned long i = 0; i < count; ++i) {
      unsigned long removed = 0;
      unsigned long clean = filterTxFlags(g_cfg, msgs[i].TxFlags, &removed);
      // A FUNC_ADDR bit on the individual message is authoritative. The same
      // bit on PassThruConnect only describes the channel's intended use; SDA
      // also sends physical requests (for example to 0x736) through that
      // channel, so only infer functional addressing for the 0x7DF broadcast.
      const bool functionalMessage =
          (msgs[i].TxFlags & j2534::dice::FUNC_ADDR) != 0 ||
          (functionalChannel && isFunctionalBroadcast(msgs[i]));
      if (functionalMessage &&
          (msgs[i].ProtocolID == ISO15765 || msgs[i].ProtocolID == ISO15765_PS))
        clean |= ISO15765_ADDR_TYPE;
      if (!removed && clean == msgs[i].TxFlags)
        continue;
      if (removed)
        SHIM_TRACE("%s: translated vendor TxFlags 0x%08lX on message %lu",
                   fn, removed, i);
      saved_.push_back({i, msgs[i].TxFlags});
      msgs[i].TxFlags = clean;
    }
  }

  ~TxFlagScrubber() {
    for (const auto &s : saved_)
      msgs_[s.index].TxFlags = s.flags;
  }

private:
  struct Saved {
    unsigned long index;
    unsigned long flags;
  };
  PASSTHRU_MSG *msgs_;
  std::vector<Saved> saved_;
};

// --- MS channel translation -------------------------------------------------

// Channels whose protocol id was rewritten on the way to the target. SDA keeps
// talking in terms of the protocol it asked for, so every message crossing the
// boundary has to be translated back and forth.
struct ChannelMap {
  unsigned long device;
  unsigned long hostProtocol;
  unsigned long targetProtocol;
  bool functional;
  bool d2 = false;
  std::shared_ptr<d2::D2ChannelState> d2State;
};
std::map<unsigned long, ChannelMap> g_channels;

std::shared_ptr<d2::D2ChannelState> d2ChannelSnapshot(unsigned long channel) {
  Guard g;
  const auto it = g_channels.find(channel);
  if (it == g_channels.end() || !it->second.d2)
    return {};
  return it->second.d2State;
}

bool channelFunctional(unsigned long channel) {
  Guard g;
  const auto it = g_channels.find(channel);
  return it != g_channels.end() && it->second.functional;
}

bool isTranslated(unsigned long channel) {
  Guard g;
  const auto it = g_channels.find(channel);
  return it != g_channels.end() &&
         it->second.hostProtocol != it->second.targetProtocol;
}

unsigned long messageCanId(const PASSTHRU_MSG &msg) {
  if (msg.DataSize < 4)
    return 0;
  return (static_cast<unsigned long>(msg.Data[0]) << 24) |
         (static_cast<unsigned long>(msg.Data[1]) << 16) |
         (static_cast<unsigned long>(msg.Data[2]) << 8) | msg.Data[3];
}

bool isActivePeriodicTxIndication(unsigned long channel,
                                  const PASSTHRU_MSG &msg) {
  if ((msg.RxStatus & TX_MSG_TYPE) == 0 || msg.DataSize < 4)
    return false;
  const unsigned long id = messageCanId(msg);
  Guard g;
  const auto channelIt = g_periodic.find(channel);
  if (channelIt == g_periodic.end())
    return false;
  for (const auto &entry : channelIt->second) {
    const PeriodicMessage &periodic = entry.second;
    if (periodic.protocol == msg.ProtocolID && periodic.canId == id)
      return true;
  }
  return false;
}

bool firstPeriodicSuppression(unsigned long channel, unsigned long canId) {
  Guard g;
  return g_periodicSuppressionLogged.insert({channel, canId}).second;
}

// The pin-switched counterpart of a protocol, or 0 when there is none.
unsigned long pinSwitchedForm(unsigned long protocol) {
  switch (protocol) {
  case ISO15765:
    return ISO15765_PS;
  case CAN:
    return CAN_PS;
  case ISO14230:
    return ISO14230_PS;
  default:
    return 0;
  }
}

bool isD2Protocol(unsigned long protocol) {
  return protocol == j2534::dice::CAN_XON_XOFF_STMIN;
}

bool d2CapabilityError(long status) {
  return status == ERR_INVALID_FLAGS || status == ERR_INVALID_PROTOCOL_ID ||
         status == ERR_NOT_SUPPORTED;
}

unsigned long d2TargetFlags(unsigned long flags, unsigned long protocol) {
  unsigned long removed = 0;
  unsigned long result = filterConnectFlags(g_cfg, flags, &removed);
  result |= CAN_29BIT_ID;
  if (protocol == CAN_PS)
    result &= ~static_cast<unsigned long>(PHYSICAL_CHANNEL);
  return result;
}

unsigned long d2TargetTxFlags(unsigned long hostTxFlags) {
  unsigned long removed = 0;
  unsigned long result = filterTxFlags(g_cfg, hostTxFlags, &removed);
  // D2 always uses 29-bit identifiers. Connect Flags are intentionally not
  // copied here: PHYSICAL_CHANNEL is a channel-selection bit, not a message
  // TxFlag. CAN_FRAME_RAW is a DiCE request for shim-side framing and must also
  // stay on the host side of the translation boundary.
  result |= CAN_29BIT_ID;
  result &= ~static_cast<unsigned long>(j2534::dice::CAN_FRAME_RAW);
  result &= ~0xF0000000UL;
  return result;
}

bool d2IsChannel(unsigned long channel) {
  Guard g;
  const auto it = g_channels.find(channel);
  return it != g_channels.end() && it->second.d2;
}

void setD2TargetError(const Target &target, const char *operation, long status) {
  char targetError[80] = {};
  if (target.GetLastError_)
    target.GetLastError_(targetError);
  std::string message = std::string(SHIM_PRODUCT_NAME) + ": D2 " + operation +
                        " failed (" + errorName(status) + ")";
  if (targetError[0])
    message += ": " + std::string(targetError);
  setLastError(message);
}

void writeCanId(unsigned char *data, uint32_t canId) {
  data[0] = static_cast<unsigned char>((canId >> 24) & 0xFF);
  data[1] = static_cast<unsigned char>((canId >> 16) & 0xFF);
  data[2] = static_cast<unsigned char>((canId >> 8) & 0xFF);
  data[3] = static_cast<unsigned char>(canId & 0xFF);
}

bool readD2LogicalMessage(const PASSTHRU_MSG &input, d2::D2LogicalMessage *message,
                          std::string *error) {
  if (input.DataSize < 5 || input.DataSize > sizeof(input.Data)) {
    if (error)
      *error = "DataSize must contain a CAN id and a non-empty payload";
    return false;
  }
  message->canId = (static_cast<uint32_t>(input.Data[0]) << 24) |
                   (static_cast<uint32_t>(input.Data[1]) << 16) |
                   (static_cast<uint32_t>(input.Data[2]) << 8) |
                   input.Data[3];
  if (message->canId > d2::kMaxCanId) {
    if (error)
      *error = "logical CAN id is outside the 29-bit range";
    return false;
  }
  message->payload.assign(input.Data + 4, input.Data + input.DataSize);
  if (message->payload.empty()) {
    if (error)
      *error = "logical D2 payload is empty";
    return false;
  }
  return true;
}

bool encodeD2HostMessage(const PASSTHRU_MSG &input,
                         d2::D2LogicalMessage *logical,
                         std::vector<d2::D2CanFrame> *frames,
                         bool *raw,
                         std::string *error) {
  if (!logical || !frames || !raw) {
    if (error)
      *error = "D2 conversion output is null";
    return false;
  }
  if (!readD2LogicalMessage(input, logical, error))
    return false;

  *raw = (input.TxFlags & j2534::dice::CAN_FRAME_RAW) != 0;
  if (*raw) {
    if (logical->payload.size() > d2::kCanPayloadSize) {
      if (error)
        *error = "D2 raw message exceeds one CAN frame";
      return false;
    }
    d2::D2CanFrame frame;
    frame.canId = logical->canId;
    frame.dataSize = static_cast<uint8_t>(logical->payload.size());
    frame.data.fill(0);
    std::copy(logical->payload.begin(), logical->payload.end(), frame.data.begin());
    frames->assign(1, frame);
    return true;
  }

  return d2::D2Segmenter::encode(*logical, frames, error);
}

PASSTHRU_MSG makeD2TargetMessage(const d2::D2CanFrame &frame,
                                 unsigned long protocol,
                                 unsigned long flags) {
  PASSTHRU_MSG result = {};
  result.ProtocolID = protocol;
  result.TxFlags = flags;
  result.DataSize = 4 + static_cast<unsigned long>(frame.dataSize);
  writeCanId(result.Data, frame.canId);
  std::copy(frame.data.begin(), frame.data.begin() + frame.dataSize,
            result.Data + 4);
  return result;
}

// J2534 periodic registration cannot represent a logical message that expands
// to multiple wire frames. This worker owns that registration and sends one
// complete D2 sequence per requested interval. It is stopped and joined on
// StopPeriodicMsg, Disconnect, and Close; it never outlives the channel state.
class D2PeriodicScheduler {
public:
  D2PeriodicScheduler(const Target &target,
                      const std::shared_ptr<d2::D2ChannelState> &state,
                      unsigned long channel,
                      std::vector<PASSTHRU_MSG> frames,
                      unsigned long interval)
      : target_(target), state_(state), channel_(channel),
        frames_(std::move(frames)), interval_(interval) {}

  ~D2PeriodicScheduler() { stop(); }

  void start() { thread_ = std::thread(&D2PeriodicScheduler::run, this); }

  // Returns the worker's last write status, not the outcome of stopping. The
  // worker runs between host calls, so a failed frame has no call to be
  // reported on at the time it happens; stopping is the first opportunity. The
  // host therefore sees a stale error even though the stop itself succeeded.
  // The alternative - always answering STATUS_NOERROR - loses the failure
  // entirely, which is worse: SDA would keep believing the periodic ran for as
  // long as it was registered.
  long stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopRequested_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
      thread_.join();
    return status_;
  }

private:
  bool waitUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return wake_.wait_until(lock, deadline,
                            [this] { return stopRequested_; });
  }

  bool stopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopRequested_;
  }

  void fail(long status, size_t frameIndex) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_ = status;
      stopRequested_ = true;
    }
    SHIM_ERROR("D2 periodic target write failed at frame %zu/%zu (%s)",
               frameIndex + 1, frames_.size(), err2str(status));
    wake_.notify_all();
  }

  void run() {
    auto nextCycle = std::chrono::steady_clock::now();
    for (;;) {
      if (waitUntil(nextCycle))
        return;

      {
        std::unique_lock<std::mutex> stateLock(state_->mutex);
        for (size_t i = 0; i < frames_.size(); ++i) {
          if (stopping())
            return;
          unsigned long count = 1;
          const long r = target_.WriteMsgs(channel_, &frames_[i], &count, 0);
          if (r != STATUS_NOERROR || count != 1) {
            fail(r == STATUS_NOERROR ? ERR_FAILED : r, i);
            return;
          }
          state_->timing.markFrameSent(std::chrono::steady_clock::now());
          if (i + 1 < frames_.size() &&
              waitUntil(state_->timing.nextFrameTime()))
            return;
        }
      }

      nextCycle += std::chrono::milliseconds(interval_);
      const auto now = std::chrono::steady_clock::now();
      if (nextCycle < now)
        nextCycle = now + std::chrono::milliseconds(interval_);
    }
  }

  Target target_;
  std::shared_ptr<d2::D2ChannelState> state_;
  unsigned long channel_;
  std::vector<PASSTHRU_MSG> frames_;
  unsigned long interval_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopRequested_ = false;
  long status_ = STATUS_NOERROR;
  std::thread thread_;
};

unsigned long allocateShimPeriodicId() {
  static unsigned long nextId = 0x80000000UL;
  Guard g;
  for (;;) {
    const unsigned long candidate = nextId++;
    if (candidate != 0 &&
        std::none_of(g_periodic.begin(), g_periodic.end(),
                     [candidate](const auto &channel) {
                       return channel.second.count(candidate) != 0;
                     }))
      return candidate;
  }
}

void removePeriodicRegistration(unsigned long channelId,
                                unsigned long messageId) {
  Guard g;
  const auto channel = g_periodic.find(channelId);
  if (channel == g_periodic.end())
    return;
  const auto periodic = channel->second.find(messageId);
  if (periodic == channel->second.end())
    return;
  const unsigned long canId = periodic->second.canId;
  channel->second.erase(periodic);
  if (channel->second.empty()) {
    g_periodic.erase(channel);
    g_periodicSuppressionLogged.erase({channelId, canId});
    return;
  }
  if (std::none_of(channel->second.begin(), channel->second.end(),
                   [canId](const auto &entry) {
                     return entry.second.canId == canId;
                   }))
    g_periodicSuppressionLogged.erase({channelId, canId});
}

d2::D2PendingRx pendingD2Message(const PASSTHRU_MSG &message) {
  d2::D2PendingRx pending;
  pending.sourceDataSize = message.DataSize;
  pending.protocol = message.ProtocolID;
  pending.rxStatus = message.RxStatus;
  pending.txFlags = message.TxFlags;
  pending.timestamp = message.Timestamp;
  pending.extraDataIndex = message.ExtraDataIndex;
  pending.frame.data.fill(0);
  pending.frame.dataSize = 0;
  if (message.DataSize < 4)
    return pending;
  pending.frame.canId = (static_cast<uint32_t>(message.Data[0]) << 24) |
                        (static_cast<uint32_t>(message.Data[1]) << 16) |
                        (static_cast<uint32_t>(message.Data[2]) << 8) |
                        message.Data[3];
  const size_t payloadSize = std::min<size_t>(message.DataSize - 4,
                                              d2::kCanPayloadSize);
  pending.frame.dataSize = static_cast<uint8_t>(payloadSize);
  std::copy(message.Data + 4, message.Data + 4 + payloadSize,
            pending.frame.data.begin());
  return pending;
}

bool isD2TxIndication(const d2::D2PendingRx &message,
                      const d2::D2ChannelState &state,
                      const PASSTHRU_MSG &sent) {
  return (message.rxStatus & TX_MSG_TYPE) != 0 &&
         message.protocol == state.targetProtocol &&
         message.sourceDataSize >= 4 && message.sourceDataSize <= 12 &&
         d2::matchesTxIndication(message, [&sent] {
           d2::D2CanFrame frame;
           frame.canId = messageCanId(sent);
           frame.data.fill(0);
           frame.dataSize = sent.DataSize >= 4
                                ? static_cast<uint8_t>(sent.DataSize - 4)
                                : 0;
           if (frame.dataSize <= d2::kCanPayloadSize)
             std::copy(sent.Data + 4, sent.Data + sent.DataSize,
                       frame.data.begin());
           return frame;
         }());
}

bool d2FilterAccepts(const d2::D2ChannelState &state, uint32_t canId) {
  bool hasPassFilter = false;
  bool passMatched = false;
  for (const auto &entry : state.filters) {
    const d2::D2ChannelState::Filter &filter = entry.second;
    const bool match = (canId & filter.mask) == (filter.pattern & filter.mask);
    if (filter.type == BLOCK_FILTER && match)
      return false;
    if (filter.type == PASS_FILTER) {
      hasPassFilter = true;
      passMatched = passMatched || match;
    }
  }
  return !hasPassFilter || passMatched;
}

long finishD2Read(const std::shared_ptr<d2::D2ChannelState> &state,
                  unsigned long *pNumMsgs, unsigned long received,
                  long status, const std::string &message) {
  *pNumMsgs = received;
  if (status == STATUS_NOERROR)
    return status;
  state->deferredErrors.push_back({status, message});
  SHIM_INFO("D2 RX: deferred error %s received=%lu depth=%zu",
            err2str(status), received, state->deferredErrors.size());
  if (received != 0) {
    return STATUS_NOERROR;
  }
  const d2::D2ChannelState::DeferredError error =
      state->deferredErrors.front();
  state->deferredErrors.pop_front();
  if (!error.message.empty())
    setLastError(error.message);
  return error.status;
}

bool hasActiveD2Assembly(const d2::D2ChannelState &state) {
  return std::any_of(
      state.reassemblers.begin(), state.reassemblers.end(),
      [](const auto &entry) { return entry.second.active(); });
}

void clearD2RxState(const std::shared_ptr<d2::D2ChannelState> &state) {
  if (!state)
    return;
  std::lock_guard<std::mutex> lock(state->mutex);
  state->pendingRx.clear();
  state->reassemblers.clear();
  state->rawExpectations.clear();
  state->deferredErrors.clear();
}

void waitForD2Frame(const d2::D2Timing &timing) {
  const auto deadline = timing.nextFrameTime();
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return;
    const auto remaining = deadline - now;
    if (remaining > std::chrono::milliseconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    else
      std::this_thread::yield();
  }
}

long waitD2TxIndication(const Target &target,
                        const std::shared_ptr<d2::D2ChannelState> &state,
                        const PASSTHRU_MSG &sent,
                        std::chrono::steady_clock::time_point deadline);

long d2WriteMsgs(const Target &target,
                 const std::shared_ptr<d2::D2ChannelState> &state,
                 PASSTHRU_MSG *msgs, unsigned long *pNumMsgs,
                 unsigned long timeout) {
  if (!pNumMsgs)
    return ERR_NULL_PARAMETER;
  const unsigned long requested = *pNumMsgs;
  *pNumMsgs = 0;
  if (requested == 0)
    return ERR_BUFFER_EMPTY;
  if (!msgs)
    return ERR_NULL_PARAMETER;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout);
  std::unique_lock<std::mutex> lock(state->mutex);
  for (unsigned long i = 0; i < requested; ++i) {
    if (msgs[i].ProtocolID != state->hostProtocol) {
      setLastError(SHIM_PRODUCT_NAME ": D2 message protocol does not match the channel");
      return ERR_MSG_PROTOCOL_ID;
    }
    d2::D2LogicalMessage logical;
    std::string validationError;
    bool rawRequested = false;
    std::vector<d2::D2CanFrame> frames;
    if (!encodeD2HostMessage(msgs[i], &logical, &frames, &rawRequested,
                             &validationError)) {
      SHIM_ERROR("D2 invalid logical message: %s", validationError.c_str());
      setLastError(std::string(SHIM_PRODUCT_NAME) + ": D2 invalid logical message: " +
                   validationError);
      return ERR_INVALID_MSG;
    }
    if (rawRequested) {
      SHIM_TRACE("D2 TX: forwarding CAN_FRAME_RAW without D2 transport framing");
    } else {
      SHIM_TRACE("D2 TX: logical message segmented into %zu CAN frames", frames.size());
    }
    const unsigned long targetTxFlags = d2TargetTxFlags(msgs[i].TxFlags);

    for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
      const auto now = std::chrono::steady_clock::now();
      if (timeout != 0 && now >= deadline) {
        SHIM_ERROR("D2 target write timed out at frame %zu/%zu", frameIndex + 1,
                   frames.size());
        return ERR_TIMEOUT;
      }
      waitForD2Frame(state->timing);
      PASSTHRU_MSG wire = makeD2TargetMessage(
          frames[frameIndex], state->targetProtocol, targetTxFlags);
      unsigned long one = 1;
      const auto remainingNow = std::chrono::steady_clock::now();
      const unsigned long remaining =
          timeout == 0 || remainingNow >= deadline
              ? 0
              : static_cast<unsigned long>(std::chrono::duration_cast<
                                                std::chrono::milliseconds>(
                                                deadline - remainingNow)
                                                .count());
      const long r = target.WriteMsgs(state->channelId, &wire, &one, remaining);
      if (r != STATUS_NOERROR || one != 1) {
        SHIM_ERROR("D2 target write failed at frame %zu/%zu (%s)",
                   frameIndex + 1, frames.size(), err2str(r));
        if (r != STATUS_NOERROR)
          setD2TargetError(target, "target write", r);
        return r == STATUS_NOERROR ? ERR_TIMEOUT : r;
      }
      state->timing.markFrameSent(std::chrono::steady_clock::now());
      const bool intermediate = frameIndex > 0 && frameIndex + 1 < frames.size();
      if (intermediate && !state->ignoreIntermediateTxAck) {
        const long ack = waitD2TxIndication(target, state, wire, deadline);
        if (ack != STATUS_NOERROR) {
          SHIM_ERROR("D2 target TX indication failed at frame %zu/%zu (%s)",
                     frameIndex + 1, frames.size(), err2str(ack));
          if (ack != ERR_TIMEOUT)
            setD2TargetError(target, "target TX indication", ack);
          return ack;
        }
      }
    }
    if (rawRequested) {
      state->rawExpectations.push_back(
          {logical.canId, timeout == 0
                              ? std::chrono::steady_clock::now() +
                                    std::chrono::seconds(1)
                              : deadline});
    }
    ++*pNumMsgs;
  }
  return STATUS_NOERROR;
}

long waitD2TxIndication(const Target &target,
                        const std::shared_ptr<d2::D2ChannelState> &state,
                        const PASSTHRU_MSG &sent,
                        std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return ERR_TIMEOUT;
    for (auto it = state->pendingRx.begin(); it != state->pendingRx.end(); ++it) {
      if (!isD2TxIndication(*it, *state, sent))
        continue;
      state->pendingRx.erase(it);
      return STATUS_NOERROR;
    }
    unsigned long remaining = static_cast<unsigned long>(std::chrono::duration_cast<
        std::chrono::milliseconds>(deadline - now).count());
    PASSTHRU_MSG indication = {};
    unsigned long one = 1;
    const long r = target.ReadMsgs(state->channelId, &indication, &one, remaining);
    if (r != STATUS_NOERROR && r != ERR_TIMEOUT && r != ERR_BUFFER_EMPTY)
      return r;
    if (one == 0)
      continue;
    const d2::D2PendingRx pending = pendingD2Message(indication);
    if (isD2TxIndication(pending, *state, sent))
      return STATUS_NOERROR;
    state->pendingRx.push_back(pending);
  }
}

long d2ReadMsgs(const Target &target,
                const std::shared_ptr<d2::D2ChannelState> &state,
                PASSTHRU_MSG *output, unsigned long *pNumMsgs,
                unsigned long timeout) {
  if (!pNumMsgs)
    return ERR_NULL_PARAMETER;
  const unsigned long capacity = *pNumMsgs;
  *pNumMsgs = 0;
  if (capacity == 0)
    return ERR_BUFFER_EMPTY;
  if (!output)
    return ERR_NULL_PARAMETER;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout);
  // D2 is serialized per channel: a blocking target read owns this channel's
  // transport state until its deadline. Other channels remain independent.
  std::unique_lock<std::mutex> lock(state->mutex);
  if (!state->deferredErrors.empty()) {
    // A message that became available after the hard error still has priority
    // over the deferred status. Probe the target without waiting; if there is
    // no ready frame, deliver the saved error now.
    if (state->pendingRx.empty() && !hasActiveD2Assembly(*state)) {
      PASSTHRU_MSG raw = {};
      unsigned long one = 1;
      const long probe = target.ReadMsgs(state->channelId, &raw, &one, 0);
      if (one != 0) {
        state->pendingRx.push_back(pendingD2Message(raw));
      } else {
        const d2::D2ChannelState::DeferredError error =
            state->deferredErrors.front();
        state->deferredErrors.pop_front();
        SHIM_INFO("D2 RX: returning deferred error %s remaining=%zu",
                  err2str(error.status), state->deferredErrors.size());
        *pNumMsgs = 0;
        if (!error.message.empty())
          setLastError(error.message);
        return error.status == STATUS_NOERROR ? probe : error.status;
      }
    }
  }
  unsigned long received = 0;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    bool assemblyExpired = false;
    for (auto &entry : state->reassemblers) {
      if (entry.second.expire(now).result == d2::D2FeedResult::timeout)
        assemblyExpired = true;
    }
    for (auto it = state->rawExpectations.begin();
         it != state->rawExpectations.end();) {
      if (it->expiresAt != std::chrono::steady_clock::time_point{} &&
          now >= it->expiresAt)
        it = state->rawExpectations.erase(it);
      else
        ++it;
    }
    if (assemblyExpired) {
      return finishD2Read(state, pNumMsgs, received, ERR_TIMEOUT,
                          SHIM_PRODUCT_NAME ": D2 assembly timeout");
    }
    if (timeout != 0 && now >= deadline) {
      *pNumMsgs = received;
      return received ? STATUS_NOERROR : ERR_TIMEOUT;
    }

    d2::D2PendingRx pending;
    long r = STATUS_NOERROR;
    bool partialTargetRead = false;
    if (!state->pendingRx.empty()) {
      pending = state->pendingRx.front();
      state->pendingRx.pop_front();
    } else {
      auto readDeadline = deadline;
      for (const auto &entry : state->reassemblers) {
        if (entry.second.active() && entry.second.deadline() < readDeadline)
          readDeadline = entry.second.deadline();
      }
      const auto readNow = std::chrono::steady_clock::now();
      const unsigned long remaining =
          timeout == 0 || readNow >= readDeadline
              ? 0
              : static_cast<unsigned long>(std::chrono::duration_cast<
                                                std::chrono::milliseconds>(
                                                    readDeadline - readNow)
                                                    .count());
      PASSTHRU_MSG raw = {};
      unsigned long one = 1;
      r = target.ReadMsgs(state->channelId, &raw, &one, remaining);
      if (r != STATUS_NOERROR && r != ERR_TIMEOUT && r != ERR_BUFFER_EMPTY) {
        char targetError[80] = {};
        if (target.GetLastError_)
          target.GetLastError_(targetError);
        std::string message = std::string(SHIM_PRODUCT_NAME) +
                              ": D2 target read failed (" + errorName(r) + ")";
        if (targetError[0])
          message += ": " + std::string(targetError);
        return finishD2Read(state, pNumMsgs, received, r, message);
      }
      partialTargetRead = r == ERR_TIMEOUT && one != 0;
      if (one == 0) {
        *pNumMsgs = received;
        const auto afterRead = std::chrono::steady_clock::now();
        if (timeout == 0)
          return received ? STATUS_NOERROR : ERR_BUFFER_EMPTY;
        const bool assemblyDeadlineReached =
            readDeadline < deadline && afterRead >= readDeadline;
        if (afterRead >= deadline || afterRead >= readDeadline) {
          if (assemblyDeadlineReached) {
            for (auto &entry : state->reassemblers)
              entry.second.expire(afterRead);
            return finishD2Read(state, pNumMsgs, received, ERR_TIMEOUT,
                                SHIM_PRODUCT_NAME ": D2 assembly timeout");
          }
          return received ? STATUS_NOERROR : ERR_TIMEOUT;
        }
        continue;
      }
      pending = pendingD2Message(raw);
    }

    if (pending.rxStatus & TX_MSG_TYPE)
      continue;
    if (pending.protocol != state->targetProtocol) {
      SHIM_TRACE("D2 RX: ignoring protocol 0x%08lX on target channel",
                 pending.protocol);
      continue;
    }
    if (pending.sourceDataSize < 4)
      continue;
    // Apply logical filters before creating or advancing a reassembler. A
    // busy CAN bus must not allocate state or turn an unrelated frame into a
    // protocol error for the host.
    if (!d2FilterAccepts(*state, pending.frame.canId))
      continue;
    if (pending.sourceDataSize < 5 || pending.sourceDataSize > 12) {
      return finishD2Read(
          state, pNumMsgs, received, ERR_INVALID_MSG,
          SHIM_PRODUCT_NAME ": D2 invalid target frame size");
    }
    auto reassemblerIt = state->reassemblers.find(pending.frame.canId);
    const auto rawExpectation = std::find_if(
        state->rawExpectations.begin(), state->rawExpectations.end(),
        [&pending](const d2::D2ChannelState::RawExpectation &expectation) {
          return expectation.responseCanId == pending.frame.canId;
        });
    const bool forceRaw = rawExpectation != state->rawExpectations.end();
    // An unknown header is not enough evidence to reinterpret a bus frame as
    // a logical raw response. Only an explicit CAN_FRAME_RAW request creates
    // that expectation; otherwise unrelated traffic is ignored.
    if (reassemblerIt == state->reassemblers.end() && !forceRaw &&
        !d2::isTransportStartFrame(pending.frame))
      continue;
    if (reassemblerIt == state->reassemblers.end()) {
      state->reassemblers.emplace(pending.frame.canId, d2::D2Reassembler());
      reassemblerIt = state->reassemblers.find(pending.frame.canId);
    }
    d2::D2Reassembler &reassembler = reassemblerIt->second;
    const d2::D2Feed feed =
        reassembler.push(pending.frame, std::chrono::steady_clock::now(),
                         forceRaw);
    if (forceRaw && feed.result != d2::D2FeedResult::incomplete)
      state->rawExpectations.erase(rawExpectation);
    if (feed.result != d2::D2FeedResult::incomplete)
      state->reassemblers.erase(reassemblerIt);
    if (feed.result == d2::D2FeedResult::incomplete)
      continue;
    if (feed.result == d2::D2FeedResult::ignored)
      continue;
    if (feed.result != d2::D2FeedResult::complete) {
      const char *reason = feed.error.empty() ? "invalid D2 frame" : feed.error.c_str();
      SHIM_ERROR("D2 RX: %s", reason);
      const long status = feed.result == d2::D2FeedResult::timeout
                              ? ERR_TIMEOUT
                              : ERR_INVALID_MSG;
      return finishD2Read(state, pNumMsgs, received, status,
                          std::string(SHIM_PRODUCT_NAME) + ": D2 " + reason);
    }
    PASSTHRU_MSG &host = output[received];
    constexpr size_t kD2PaddedPayloadSize = 8;
    if (feed.message.payload.size() > d2::kDefaultMaxPayloadSize ||
        feed.message.payload.size() > sizeof(host.Data) - 4) {
      return finishD2Read(
          state, pNumMsgs, received, ERR_INVALID_MSG,
          SHIM_PRODUCT_NAME ": D2 response exceeds the host payload contract");
    }
    ++received;
    host = {};
    host.ProtocolID = state->hostProtocol;
    host.RxStatus = pending.rxStatus;
    host.TxFlags = pending.txFlags;
    host.Timestamp = pending.timestamp;
    host.ExtraDataIndex = pending.extraDataIndex;
    // DiCE pads short logical payloads to eight bytes, while longer D2
    // messages are exposed at their actual size. host.Data was zeroed above,
    // so the padding remains deterministic without truncating valid RX data.
    host.DataSize = 4 + static_cast<unsigned long>(
                              std::max(feed.message.payload.size(),
                                       kD2PaddedPayloadSize));
    writeCanId(host.Data, feed.message.canId);
    std::copy(feed.message.payload.begin(), feed.message.payload.end(),
              host.Data + 4);
    SHIM_TRACE("D2 RX: completed logical message from CAN frames");
    *pNumMsgs = received;
    if (received == capacity)
      return STATUS_NOERROR;
    if (partialTargetRead)
      return STATUS_NOERROR;
    if (timeout == 0)
      return STATUS_NOERROR;
  }
}

// Rewrites PASSTHRU_MSG::ProtocolID for a translated channel and restores the
// caller's buffer afterwards, exactly like TxFlagScrubber.
class MsgProtocolScrubber {
public:
  MsgProtocolScrubber(unsigned long channel, PASSTHRU_MSG *msgs, unsigned long count)
      : msgs_(msgs) {
    if (!msgs || !count)
      return;
    Guard g;
    const auto it = g_channels.find(channel);
    if (it == g_channels.end() ||
        it->second.hostProtocol == it->second.targetProtocol)
      return;
    for (unsigned long i = 0; i < count; ++i) {
      if (msgs[i].ProtocolID != it->second.hostProtocol)
        continue;
      saved_.push_back({i, msgs[i].ProtocolID});
      msgs[i].ProtocolID = it->second.targetProtocol;
    }
  }

  ~MsgProtocolScrubber() {
    for (const auto &s : saved_)
      msgs_[s.index].ProtocolID = s.protocol;
  }

private:
  struct Saved {
    unsigned long index;
    unsigned long protocol;
  };
  PASSTHRU_MSG *msgs_;
  std::vector<Saved> saved_;
};

// --- CLEAR_RX_BUFFER emulation ----------------------------------------------

// Drains the channel's receive queue by reading it dry, for targets that do not
// implement CLEAR_RX_BUFFER. Returns STATUS_NOERROR once the queue is empty.
//
// This is the one place the shim reports success for a *standard* ioctl the
// target refused, rather than for a vendor id. It is defensible only because
// the postcondition SDA depends on is genuinely established: after this call
// the queue really is empty. If the drain cannot finish, the target's original
// error is returned unchanged.
long drainRxQueue(const Target &target, unsigned long channel, long targetStatus) {
  // A PASSTHRU_MSG is 4152 bytes, so the batch lives on the heap. The iteration
  // cap stops a channel that is receiving faster than we can read from turning
  // this into an unbounded loop inside a call SDA expects to be instant.
  constexpr unsigned long kBatch = 8;
  constexpr int kMaxRounds = 64;

  std::vector<PASSTHRU_MSG> batch(kBatch);
  unsigned long drained = 0;
  for (int round = 0; round < kMaxRounds; ++round) {
    unsigned long n = kBatch;
    // Timeout 0: return whatever is already buffered, do not wait for traffic.
    const long r = target.ReadMsgs(channel, batch.data(), &n, 0);
    if (r != STATUS_NOERROR && r != ERR_TIMEOUT && r != ERR_BUFFER_EMPTY) {
      SHIM_ERROR("CLEAR_RX_BUFFER: target refused (%s) and the drain failed too "
                 "(%s) after %lu message(s)",
                 errorName(targetStatus), errorName(r), drained);
      return targetStatus;
    }
    drained += n;
    // A short successful read is only a snapshot. Traffic may have arrived
    // between the driver's dequeue and our return (notably periodic
    // TesterPresent), so require an explicit empty result on the next probe.
    if (n == 0 || r == ERR_BUFFER_EMPTY)
      return STATUS_NOERROR;
  }

  SHIM_ERROR("CLEAR_RX_BUFFER: drained %lu message(s) and the queue is still not "
             "empty, reporting the target's %s",
             drained, errorName(targetStatus));
  return targetStatus;
}

// --- ISO15765_SWDL_FABRICATE instrumentation --------------------------------

void noteFabricate(unsigned long channel, bool on) {
  Guard g;
  if (on)
    g_fabricateChannels.insert(channel);
  else
    g_fabricateChannels.erase(channel);
}

bool fabricateActive(unsigned long channel) {
  Guard g;
  return g_fabricateChannels.count(channel) != 0;
}

void stopShimPeriodic(unsigned long channel) {
  std::vector<std::shared_ptr<D2PeriodicScheduler>> schedulers;
  {
    Guard g;
    const auto it = g_periodic.find(channel);
    if (it == g_periodic.end())
      return;
    for (const auto &entry : it->second) {
      if (entry.second.scheduler)
        schedulers.push_back(entry.second.scheduler);
    }
  }
  for (const auto &scheduler : schedulers)
    scheduler->stop();
}

void forgetChannel(unsigned long channel) {
  stopShimPeriodic(channel);
  Guard g;
  g_fabricateChannels.erase(channel);
  g_periodic.erase(channel);
  for (auto it = g_periodicSuppressionLogged.begin();
       it != g_periodicSuppressionLogged.end();) {
    if (it->first == channel)
      it = g_periodicSuppressionLogged.erase(it);
    else
      ++it;
  }
  g_channels.erase(channel);
}

// PassThruClose invalidates every channel of the device without the host having
// to disconnect them, and SDA does exactly that on its error paths: the live
// trace shows 50 successful Connects against 41 Disconnects. Drivers hand out
// small channel ids from a per-open counter, so the next session gets the same
// numbers back - id 3 was reissued five times in one log. A left-over entry
// would then make the shim rewrite ProtocolID on a channel that was never
// translated, and every message would come back ERR_MSG_PROTOCOL_ID.
void forgetDeviceChannels(unsigned long device) {
  std::vector<unsigned long> channels;
  {
    Guard g;
    for (const auto &entry : g_channels) {
      if (entry.second.device == device)
        channels.push_back(entry.first);
    }
  }
  for (const unsigned long channel : channels)
    stopShimPeriodic(channel);

  Guard g;
  size_t stale = 0;
  for (auto it = g_channels.begin(); it != g_channels.end();) {
    if (it->second.device != device) {
      ++it;
      continue;
    }
    g_fabricateChannels.erase(it->first);
    g_periodic.erase(it->first);
    for (auto logged = g_periodicSuppressionLogged.begin();
         logged != g_periodicSuppressionLogged.end();) {
      if (logged->first == it->first)
        logged = g_periodicSuppressionLogged.erase(logged);
      else
        ++logged;
    }
    it = g_channels.erase(it);
    ++stale;
  }
  if (stale)
    SHIM_INFO("PassThruClose: dropping state for %zu channel(s) that were never "
              "disconnected",
              stale);
}

// DiCE fabrication is a stateful SWDL stream. SDA may split one programming
// block across several individually valid PassThruWriteMsgs calls, so neither
// DataSize nor the ISO-TP 4095-byte limit can identify a safe chunk. Until the
// proprietary stream contract is implemented, fail the entire active session
// closed and never expose any of its chunks to the target driver.
bool fabricateWriteSupported(unsigned long channel) {
  if (!fabricateActive(channel))
    return true;
  SHIM_ERROR("FABRICATE: refusing WriteMsgs on channel %lu while the unsupported "
             "DiCE ISO15765_SWDL_FABRICATE streaming mode is active; no payload "
             "was forwarded",
             channel);
  return false;
}

bool allFunctionalBroadcasts(const PASSTHRU_MSG *msgs, unsigned long count) {
  if (!msgs || count == 0)
    return false;
  for (unsigned long i = 0; i < count; ++i) {
    if (!isFunctionalBroadcast(msgs[i]))
      return false;
  }
  return true;
}

} // namespace

// Every export is wrapped so that no C++ exception crosses the ABI boundary
// into the VB6 host. ensureInit() is inside the guard: config loading and
// logger setup allocate, and those allocations must not escape either.
// PassThruGetLastError must not clear the message it is about to report.
#define SHIM_GUARD_BEGIN_NOCLEAR                                               \
  try {                                                                        \
    ensureInit();
#define SHIM_GUARD_BEGIN                                                       \
  SHIM_GUARD_BEGIN_NOCLEAR                                                     \
  clearLastError();
#define SHIM_GUARD_END(fn)                                                     \
  }                                                                            \
  catch (const std::exception &e) {                                            \
    SHIM_ERROR("%s: exception: %s", fn, e.what());                             \
    return ERR_FAILED;                                                         \
  }                                                                            \
  catch (...) {                                                                \
    SHIM_ERROR("%s: unknown exception", fn);                                   \
    return ERR_FAILED;                                                         \
  }

extern "C" {

long WINAPI PassThruOpen(void *pName, unsigned long *pDeviceID) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruOpen [ Name:%s ]",
             pName ? static_cast<const char *>(pName) : "<null>");

  Target target;
  {
    Guard g;
    if (!g_target.loaded()) {
      std::string err;
      if (!loadTarget(g_cfg.library, &g_target, &err)) {
        SHIM_ERROR("Failed to load target J2534 driver: %s", err.c_str());
        g_lastError = std::string(SHIM_PRODUCT_NAME ": ") + err;
        return ERR_DEVICE_NOT_CONNECTED;
      }
    }
    target = g_target;
  }

  // SDA passes the Name from its registry entry. DiCE ignored it; the
  // DrewTech driver reads pName as a device selector and fails to find a
  // device by that name. J2534 v04.04 defines NULL as "first available
  // device", so that is what goes downstream by default.
  void *name = pName;
  if (!g_cfg.forwardDeviceName) {
    name = g_cfg.deviceName.empty()
               ? nullptr
               : const_cast<char *>(g_cfg.deviceName.c_str());
    if (pName)
      SHIM_INFO("PassThruOpen: replacing host device name [%s] with [%s]",
                static_cast<const char *>(pName),
                name ? static_cast<const char *>(name) : "<NULL>");
  }

  const long r = target.Open(name, pDeviceID);
  SHIM_TRACE("<<-- PassThruOpen [ DeviceId:%lu ] -> %s",
             pDeviceID ? *pDeviceID : 0UL, err2str(r));
  return r;
  SHIM_GUARD_END("PassThruOpen")
}

long WINAPI PassThruClose(unsigned long DeviceID) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruClose [ DeviceId:%lu ]", DeviceID);
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruClose");
  // A shim-owned periodic worker may be writing the target channel. Stop it
  // before the target invalidates the channel during Close.
  {
    std::vector<unsigned long> channels;
    {
      Guard g;
      for (const auto &entry : g_channels) {
        if (entry.second.device == DeviceID)
          channels.push_back(entry.first);
      }
    }
    for (const unsigned long channel : channels)
      stopShimPeriodic(channel);
  }
  const long r = target.Close(DeviceID);
  forgetDeviceChannels(DeviceID);
  SHIM_TRACE("<<-- PassThruClose -> %s", err2str(r));
  return r;
  SHIM_GUARD_END("PassThruClose")
}

long WINAPI PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                            unsigned long Flags, unsigned long Baudrate,
                            unsigned long *pChannelID) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruConnect [ DeviceId:%lu ProtocolId:%lu (%s) Flags:0x%08lX "
             "Baudrate:%lu ]",
             DeviceID, ProtocolID, protocolName(ProtocolID), Flags, Baudrate);

  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruConnect");

  if (isD2Protocol(ProtocolID) && g_cfg.d2Route != D2Route::reject) {
    if (!pChannelID)
      return ERR_NULL_PARAMETER;
    *pChannelID = 0;

    unsigned long targetProtocol =
        g_cfg.d2Route == D2Route::canPs ? CAN_PS : CAN;
    unsigned long targetFlags = d2TargetFlags(Flags, targetProtocol);
    long r = target.Connect(DeviceID, targetProtocol, targetFlags, Baudrate,
                            pChannelID);

    const bool canFallback =
        g_cfg.d2Route == D2Route::autoDetect &&
        (targetFlags & PHYSICAL_CHANNEL) != 0 && d2CapabilityError(r);
    if (canFallback) {
      if (*pChannelID)
        target.Disconnect(*pChannelID);
      *pChannelID = 0;
      targetProtocol = CAN_PS;
      targetFlags = d2TargetFlags(Flags, targetProtocol);
      SHIM_INFO("D2 connect: CAN route refused (%s), trying CAN_PS pins=0x%04lX",
                err2str(r), g_cfg.d2MsPins);
      r = target.Connect(DeviceID, targetProtocol, targetFlags, Baudrate,
                         pChannelID);
    }
    if (r != STATUS_NOERROR) {
      if (pChannelID)
        *pChannelID = 0;
      return r;
    }

    SCONFIG params[3] = {{DATA_RATE, static_cast<uint32_t>(Baudrate)},
                         {LOOPBACK, 0},
                         {J1962_PINS, static_cast<uint32_t>(g_cfg.d2MsPins)}};
    const uint32_t paramCount = targetProtocol == CAN_PS ? 3 : 2;
    SCONFIG_LIST list = {paramCount, params};
    const long configStatus = target.Ioctl(*pChannelID, SET_CONFIG, &list, nullptr);
    if (configStatus != STATUS_NOERROR) {
      setD2TargetError(target, "target setup", configStatus);
      target.Disconnect(*pChannelID);
      *pChannelID = 0;
      return configStatus;
    }

    auto state = std::make_shared<d2::D2ChannelState>();
    state->deviceId = DeviceID;
    state->channelId = *pChannelID;
    state->hostProtocol = ProtocolID;
    state->targetProtocol = targetProtocol;
    state->baudRate = Baudrate;
    state->hostFlags = Flags;
    state->targetFlags = targetFlags;
    state->pins = targetProtocol == CAN_PS ? g_cfg.d2MsPins : 0;
    state->ignoreIntermediateTxAck = g_cfg.d2IgnoreIntermediateTxAck;
    {
      Guard g;
      g_channels[*pChannelID] = {DeviceID, ProtocolID, targetProtocol,
                                 (Flags & j2534::dice::FUNC_ADDR) != 0, true,
                                 state};
    }
    SHIM_INFO("D2 channel: %s -> %s%s", protocolName(ProtocolID),
              protocolName(targetProtocol),
              targetProtocol == CAN_PS ? " with J1962_PINS" : "");
    return STATUS_NOERROR;
  }

  // A vendor protocol id must never reach the target: 0x10000001 is DiCE
  // CAN_XON_XOFF but DrewTech J1708_PS. D2 platforms (P80/P1/P2) are out of
  // scope for this shim - use VolvoTools for those.
  if (ProtocolID >= kVendorIdBase && !isStandardProtocol(ProtocolID)) {
    const Policy p = policyFor(g_cfg, Category::protocol, ProtocolID,
                               protocolName(ProtocolID), false);
    if (p != Policy::forward) {
      SHIM_ERROR("Blocked vendor ProtocolId:%lu (%s) - not forwarded to target",
                 ProtocolID, protocolName(ProtocolID));
      setLastError(SHIM_PRODUCT_NAME ": vendor protocol not supported by target driver");
      return ERR_INVALID_PROTOCOL_ID;
    }
  }

  // Flags carry vendor bits too. PHYSICAL_CHANNEL (0x20000000) is standard
  // J2534-2 and is how SDA selects the MS channel, so only 0x10000000 is cut.
  unsigned long removed = 0;
  const unsigned long flags = filterConnectFlags(g_cfg, Flags, &removed);
  if (removed)
    SHIM_ERROR("PassThruConnect: stripped vendor Flags 0x%08lX (FUNC_ADDR on DiCE "
               "== SNIFF_MODE on DrewTech), forwarding 0x%08lX",
               removed, flags);

  long r = target.Connect(DeviceID, ProtocolID, flags, Baudrate, pChannelID);

  // SDA asks for the MS bus as ISO15765 + PHYSICAL_CHANNEL. Adapters without a
  // second physical CAN refuse that, but reach the same wires through the
  // J2534-2 pin-switched protocol. Retry that way when configured.
  const bool routeUnsupported = r == ERR_INVALID_FLAGS ||
                                r == ERR_INVALID_PROTOCOL_ID ||
                                r == ERR_NOT_SUPPORTED;
  if (routeUnsupported && g_cfg.msFallback && (flags & PHYSICAL_CHANNEL)) {
    const unsigned long psProtocol = pinSwitchedForm(ProtocolID);
    if (psProtocol) {
      SHIM_INFO("MS fallback: %s + PHYSICAL_CHANNEL refused (%s), retrying as %s "
                "with J1962_PINS=0x%04lX",
                protocolName(ProtocolID), err2str(r), protocolName(psProtocol),
                g_cfg.msPins);

      const unsigned long psFlags = flags & ~static_cast<unsigned long>(PHYSICAL_CHANNEL);
      r = target.Connect(DeviceID, psProtocol, psFlags, Baudrate, pChannelID);
      if (r == STATUS_NOERROR && pChannelID) {
        SCONFIG params[] = {
            {J1962_PINS, static_cast<uint32_t>(g_cfg.msPins)},
            {LOOPBACK, 0},
        };
        SCONFIG_LIST list = {2, params};
        const long pinStatus = target.Ioctl(*pChannelID, SET_CONFIG, &list, nullptr);
        if (pinStatus != STATUS_NOERROR) {
          SHIM_ERROR("MS fallback: J1962_PINS/LOOPBACK rejected (%s), closing the "
                     "channel",
                     err2str(pinStatus));
          target.Disconnect(*pChannelID);
          *pChannelID = 0; // the id we just destroyed must not escape
          r = ERR_INVALID_FLAGS; // report the original refusal to the host
        } else {
          Guard g;
          g_channels[*pChannelID] = {
              DeviceID, ProtocolID, psProtocol,
              (Flags & j2534::dice::FUNC_ADDR) != 0,
          };
          SHIM_INFO("MS fallback: channel %lu now speaks %s to the target, %s to "
                    "the host",
                    *pChannelID, protocolName(psProtocol), protocolName(ProtocolID));
        }
      }
    }
  }

  if (r == STATUS_NOERROR && pChannelID) {
    Guard g;
    if (g_channels.count(*pChannelID) == 0) {
      g_channels[*pChannelID] = {
          DeviceID, ProtocolID, ProtocolID,
          (Flags & j2534::dice::FUNC_ADDR) != 0,
      };
    }
  }

  // A failed Connect leaves whatever the target driver happened to write in
  // *pChannelID. Reporting that id made the log read as if a channel had been
  // opened when none had.
  if (r != STATUS_NOERROR) {
    if (pChannelID)
      *pChannelID = 0;
    SHIM_TRACE("<<-- PassThruConnect [ no channel ] -> %s", err2str(r));
  } else {
    SHIM_TRACE("<<-- PassThruConnect [ ChannelId:%lu ] -> %s",
               pChannelID ? *pChannelID : 0UL, err2str(r));
  }
  return r;
  SHIM_GUARD_END("PassThruConnect")
}

long WINAPI PassThruDisconnect(unsigned long ChannelID) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruDisconnect [ ChannelId:%lu ]", ChannelID);
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruDisconnect");
  stopShimPeriodic(ChannelID);
  const long r = target.Disconnect(ChannelID);
  forgetChannel(ChannelID);
  SHIM_TRACE("<<-- PassThruDisconnect -> %s", err2str(r));
  return r;
  SHIM_GUARD_END("PassThruDisconnect")
}

long WINAPI PassThruReadMsgs(unsigned long ChannelID, void *pMsg,
                             unsigned long *pNumMsgs, unsigned long Timeout) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruReadMsgs  [ ChannelId:%lu NumMsgs:%lu Timeout:%lu ]",
             ChannelID, pNumMsgs ? *pNumMsgs : 0UL, Timeout);
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruReadMsgs");

  if (const auto d2State = d2ChannelSnapshot(ChannelID))
    return d2ReadMsgs(target, d2State, static_cast<PASSTHRU_MSG *>(pMsg),
                      pNumMsgs, Timeout);

  const unsigned long capacity = pNumMsgs ? *pNumMsgs : 0;
  const ULONGLONG deadline = GetTickCount64() + Timeout;
  long r = ERR_BUFFER_EMPTY;
  unsigned long received = 0;

  for (;;) {
    unsigned long n = capacity;
    if (pNumMsgs)
      *pNumMsgs = n;
    const ULONGLONG now = GetTickCount64();
    const unsigned long remaining =
        Timeout == 0 || now >= deadline
            ? 0
            : static_cast<unsigned long>(deadline - now);
    r = target.ReadMsgs(ChannelID, pMsg, pNumMsgs, remaining);

    const bool countIsValid =
        (r == STATUS_NOERROR || r == ERR_BUFFER_EMPTY || r == ERR_TIMEOUT);
    n = (countIsValid && pNumMsgs) ? *pNumMsgs : 0;
    if (!countIsValid)
      break;

    PASSTHRU_MSG *msgs = static_cast<PASSTHRU_MSG *>(pMsg);
    received = 0;
    for (unsigned long i = 0; i < n && msgs; ++i) {
      if (isActivePeriodicTxIndication(ChannelID, msgs[i])) {
        const unsigned long canId = messageCanId(msgs[i]);
        if (firstPeriodicSuppression(ChannelID, canId)) {
          SHIM_INFO("PassThruReadMsgs: suppressing TX indication on channel %lu "
                    "CAN ID 0x%lX; target periodic echo was filtered",
                    ChannelID, canId);
        } else {
          SHIM_TRACE("PassThruReadMsgs: suppressed repeated periodic TX indication "
                     "on channel %lu CAN ID 0x%lX",
                     ChannelID, canId);
        }
        continue;
      }
      if (received != i)
        msgs[received] = msgs[i];
      ++received;
    }
    if (pNumMsgs)
      *pNumMsgs = received;
    if (received != 0)
      break;
    if (Timeout == 0) {
      r = ERR_BUFFER_EMPTY;
      break;
    }
    if (GetTickCount64() >= deadline) {
      r = ERR_TIMEOUT;
      break;
    }
  }

  if (r == ERR_BUFFER_EMPTY && Timeout > 0 && received == 0 &&
      g_cfg.normalizeEmptyBlockingRead) {
    SHIM_INFO("PassThruReadMsgs: target returned ERR_BUFFER_EMPTY after a "
              "blocking wait; reporting ERR_TIMEOUT");
    r = ERR_TIMEOUT;
  }

  // v04.04 defines *pNumMsgs as "the number of messages actually read" for all
  // three of these. ERR_TIMEOUT in particular is the normal outcome whenever
  // fewer messages arrived than were asked for - it routinely carries data, and
  // treating it as "nothing was read" silently dropped the protocol rewrite on
  // exactly the messages a translated channel exists to deliver. Any other code
  // is a parameter error, where the count may still be the input capacity and
  // walking it would touch uninitialised slots.
  // On a translated channel the target answers with the pin-switched protocol
  // id; SDA only knows the one it asked for.
  if (pMsg && received && isTranslated(ChannelID)) {
    Guard g;
    const auto it = g_channels.find(ChannelID);
    if (it != g_channels.end()) {
      for (unsigned long i = 0; i < received; ++i) {
        PASSTHRU_MSG *m = static_cast<PASSTHRU_MSG *>(pMsg) + i;
        if (m->ProtocolID == it->second.targetProtocol)
          m->ProtocolID = it->second.hostProtocol;
      }
    }
  }

  if (Log::instance().enabled(Level::trace)) {
    const unsigned long n = received;
    for (unsigned long i = 0; i < n && pMsg; ++i) {
      const PASSTHRU_MSG *m = static_cast<const PASSTHRU_MSG *>(pMsg) + i;
      SHIM_TRACE("<<-- PassThruReadMsgs [ ChannelId:%lu ] %s", ChannelID,
                 formatMsg(m).c_str());
    }
    if (n == 0)
      SHIM_TRACE("<<-- PassThruReadMsgs [ NumMsgs:0 ] -> %s", err2str(r));
  }
  return r;
  SHIM_GUARD_END("PassThruReadMsgs")
}

long WINAPI PassThruWriteMsgs(unsigned long ChannelID, void *pMsg,
                              unsigned long *pNumMsgs, unsigned long Timeout) {
  SHIM_GUARD_BEGIN
  const unsigned long n = pNumMsgs ? *pNumMsgs : 0UL;
  const unsigned long requested = n;
  PASSTHRU_MSG *msgs = static_cast<PASSTHRU_MSG *>(pMsg);

  if (Log::instance().enabled(Level::trace)) {
    for (unsigned long i = 0; i < n && msgs; ++i)
      SHIM_TRACE("-->> PassThruWriteMsgs [ ChannelId:%lu ] %s", ChannelID,
                 formatMsg(msgs + i).c_str());
  }
  if (!fabricateWriteSupported(ChannelID)) {
    if (pNumMsgs)
      *pNumMsgs = 0;
    setLastError(SHIM_PRODUCT_NAME
                 ": ISO15765_SWDL_FABRICATE streaming mode is unsupported");
    return ERR_NOT_SUPPORTED;
  }

  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruWriteMsgs");

  if (const auto d2State = d2ChannelSnapshot(ChannelID))
    return d2WriteMsgs(target, d2State, msgs, pNumMsgs, Timeout);

  TxFlagScrubber scrub(ChannelID, msgs, n, "PassThruWriteMsgs");
  MsgProtocolScrubber proto(ChannelID, msgs, n);
  long r = target.WriteMsgs(ChannelID, pMsg, pNumMsgs, Timeout);
  if (r == ERR_TIMEOUT && g_cfg.tolerateUnackedFunctionalBroadcasts &&
      allFunctionalBroadcasts(msgs, requested)) {
    SHIM_ERROR("PassThruWriteMsgs: channel %lu target returned %s for "
               "functional 0x7DF broadcast; suppressing the error and "
               "reporting best-effort submission (delivery is not confirmed)",
               ChannelID, err2str(r));
    if (pNumMsgs)
      *pNumMsgs = requested;
    r = STATUS_NOERROR;
  }
  SHIM_TRACE("<<-- PassThruWriteMsgs [ ChannelId:%lu NumMsgs:%lu ] -> %s", ChannelID,
             pNumMsgs ? *pNumMsgs : 0UL, err2str(r));
  return r;
  SHIM_GUARD_END("PassThruWriteMsgs")
}

long WINAPI PassThruStartPeriodicMsg(unsigned long ChannelID, void *pMsg,
                                     unsigned long *pMsgID,
                                     unsigned long TimeInterval) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruStartPeriodicMsg [ ChannelId:%lu Interval:%lu ] %s",
             ChannelID, TimeInterval, formatMsg(pMsg).c_str());
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruStartPeriodicMsg");

  if (const auto d2State = d2ChannelSnapshot(ChannelID)) {
    if (!pMsg || !pMsgID)
      return ERR_NULL_PARAMETER;
    d2::D2LogicalMessage logical;
    std::vector<d2::D2CanFrame> frames;
    bool rawRequested = false;
    std::string validationError;
    if (!encodeD2HostMessage(*static_cast<PASSTHRU_MSG *>(pMsg), &logical,
                             &frames, &rawRequested, &validationError)) {
      setLastError(std::string(SHIM_PRODUCT_NAME) + ": D2 invalid periodic message: " +
                   validationError);
      return ERR_INVALID_MSG;
    }
    if (rawRequested)
      SHIM_TRACE("D2 periodic: forwarding CAN_FRAME_RAW without D2 transport framing");
    const unsigned long targetFlags = d2TargetTxFlags(
        static_cast<PASSTHRU_MSG *>(pMsg)->TxFlags);
    std::vector<PASSTHRU_MSG> wireFrames;
    wireFrames.reserve(frames.size());
    for (const auto &frame : frames)
      wireFrames.push_back(makeD2TargetMessage(frame, d2State->targetProtocol,
                                               targetFlags));

    if (wireFrames.size() == 1) {
      std::lock_guard<std::mutex> lock(d2State->mutex);
      const long r = target.StartPeriodicMsg(ChannelID, &wireFrames.front(), pMsgID,
                                             TimeInterval);
      if (r == STATUS_NOERROR) {
        Guard g;
        g_periodic[ChannelID][*pMsgID] =
            {wireFrames.front().ProtocolID, messageCanId(wireFrames.front()), {}};
      }
      return r;
    }

    if (TimeInterval == 0) {
      setLastError(SHIM_PRODUCT_NAME ": D2 periodic interval must be non-zero");
      return ERR_INVALID_TIME_INTERVAL;
    }
    std::vector<PASSTHRU_MSG> schedulerFrames = wireFrames;
    auto scheduler = std::make_shared<D2PeriodicScheduler>(
        target, d2State, ChannelID, std::move(schedulerFrames), TimeInterval);
    const unsigned long shimMessageId = allocateShimPeriodicId();
    *pMsgID = shimMessageId;
    {
      Guard g;
      g_periodic[ChannelID][shimMessageId] =
          {wireFrames.front().ProtocolID, messageCanId(wireFrames.front()), scheduler};
    }
    try {
      scheduler->start();
    } catch (...) {
      removePeriodicRegistration(ChannelID, shimMessageId);
      *pMsgID = 0;
      setLastError(SHIM_PRODUCT_NAME ": unable to start D2 periodic scheduler");
      return ERR_FAILED;
    }
    SHIM_INFO("D2 periodic: using shim scheduler for %zu wire frames every %lu ms",
              wireFrames.size(), TimeInterval);
    return STATUS_NOERROR;
  }

  TxFlagScrubber scrub(ChannelID, static_cast<PASSTHRU_MSG *>(pMsg), pMsg ? 1 : 0,
                       "PassThruStartPeriodicMsg");
  MsgProtocolScrubber proto(ChannelID, static_cast<PASSTHRU_MSG *>(pMsg),
                            pMsg ? 1 : 0);
  // Keep both scrubbers alive through the target call and registration below:
  // g_periodic intentionally stores the target protocol id while `proto` has
  // rewritten the caller's message, then the destructors restore host data.
  const long r = target.StartPeriodicMsg(ChannelID, pMsg, pMsgID, TimeInterval);
  if (r == STATUS_NOERROR && pMsg && pMsgID) {
    const PASSTHRU_MSG &msg = *static_cast<const PASSTHRU_MSG *>(pMsg);
    Guard g;
    g_periodic[ChannelID][*pMsgID] = {msg.ProtocolID, messageCanId(msg)};
  }
  SHIM_TRACE("<<-- PassThruStartPeriodicMsg [ MsgId:%lu ] -> %s",
             pMsgID ? *pMsgID : 0UL, err2str(r));
  return r;
  SHIM_GUARD_END("PassThruStartPeriodicMsg")
}

long WINAPI PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruStopPeriodicMsg [ ChannelId:%lu MsgId:%lu ]", ChannelID,
             MsgID);
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruStopPeriodicMsg");
  std::shared_ptr<D2PeriodicScheduler> scheduler;
  {
    Guard g;
    const auto channel = g_periodic.find(ChannelID);
    if (channel != g_periodic.end()) {
      const auto periodic = channel->second.find(MsgID);
      if (periodic != channel->second.end())
        scheduler = periodic->second.scheduler;
    }
  }

  // For a shim-scheduled periodic, `r` reports the worker's last write rather
  // than the stop: see D2PeriodicScheduler::stop. A non-zero value here means
  // the periodic died earlier, not that it is still running - which is why the
  // registration is removed below regardless of `r`, unlike the target case.
  const long r = scheduler ? scheduler->stop()
                           : target.StopPeriodicMsg(ChannelID, MsgID);
  if (scheduler) {
    // The worker has stopped and no target periodic registration exists, so
    // its shim registration must be removed even when a worker write failed.
    removePeriodicRegistration(ChannelID, MsgID);
  } else if (r == STATUS_NOERROR) {
    removePeriodicRegistration(ChannelID, MsgID);
  } else {
    // Keep the registration when the target failed to stop. The safer result
    // is to continue suppressing the echo than to expose a known TX indication
    // as ECU data while the target may still be transmitting it.
    SHIM_INFO("PassThruStopPeriodicMsg: target returned %s; keeping periodic "
              "suppression state for channel %lu MsgID %lu",
              err2str(r), ChannelID, MsgID);
  }
  SHIM_TRACE("<<-- PassThruStopPeriodicMsg -> %s", err2str(r));
  return r;
  SHIM_GUARD_END("PassThruStopPeriodicMsg")
}

long WINAPI PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                                   void *pMaskMsg, void *pPatternMsg,
                                   void *pFlowControlMsg, unsigned long *pMsgID) {
  SHIM_GUARD_BEGIN
  if (Log::instance().enabled(Level::trace)) {
    SHIM_TRACE("-->> PassThruStartMsgFilter Mask    %s", formatMsg(pMaskMsg).c_str());
    SHIM_TRACE("-->> PassThruStartMsgFilter Pattern %s",
               formatMsg(pPatternMsg).c_str());
    if (pFlowControlMsg)
      SHIM_TRACE("-->> PassThruStartMsgFilter Flow    %s",
                 formatMsg(pFlowControlMsg).c_str());
    SHIM_TRACE("--> PassThruStartMsgFilter [ ChannelId:%lu FilterType:%lu ]",
               ChannelID, FilterType);
  }
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruStartMsgFilter");

  if (const auto d2State = d2ChannelSnapshot(ChannelID)) {
    if (!pMaskMsg || !pPatternMsg || !pMsgID)
      return ERR_NULL_PARAMETER;
    if (FilterType != PASS_FILTER && FilterType != BLOCK_FILTER)
      return ERR_NOT_SUPPORTED;
    const PASSTHRU_MSG &mask = *static_cast<PASSTHRU_MSG *>(pMaskMsg);
    const PASSTHRU_MSG &pattern = *static_cast<PASSTHRU_MSG *>(pPatternMsg);
    if (mask.ProtocolID != d2State->hostProtocol ||
        pattern.ProtocolID != d2State->hostProtocol || mask.DataSize < 4 ||
        pattern.DataSize < 4) {
      setLastError(SHIM_PRODUCT_NAME ": D2 filter must use a logical CAN id");
      return ERR_INVALID_MSG;
    }
    const uint32_t maskId = messageCanId(mask);
    const uint32_t patternId = messageCanId(pattern);
    if (patternId > d2::kMaxCanId)
      return ERR_INVALID_MSG;
    std::lock_guard<std::mutex> lock(d2State->mutex);
    const unsigned long id = 0x80000000UL | d2State->nextFilterId++;
    d2State->filters[id] = {FilterType, maskId, patternId};
    *pMsgID = id;
    SHIM_TRACE("D2 logical filter %lu installed", id);
    return STATUS_NOERROR;
  }

  TxFlagScrubber scrubMask(ChannelID, static_cast<PASSTHRU_MSG *>(pMaskMsg),
                           pMaskMsg ? 1 : 0, "PassThruStartMsgFilter/Mask", false);
  TxFlagScrubber scrubPattern(
      ChannelID, static_cast<PASSTHRU_MSG *>(pPatternMsg), pPatternMsg ? 1 : 0,
      "PassThruStartMsgFilter/Pattern", false);
  TxFlagScrubber scrubFlow(ChannelID, static_cast<PASSTHRU_MSG *>(pFlowControlMsg),
                           pFlowControlMsg ? 1 : 0,
                           "PassThruStartMsgFilter/Flow");
  MsgProtocolScrubber protoMask(ChannelID, static_cast<PASSTHRU_MSG *>(pMaskMsg),
                                pMaskMsg ? 1 : 0);
  MsgProtocolScrubber protoPattern(ChannelID,
                                   static_cast<PASSTHRU_MSG *>(pPatternMsg),
                                   pPatternMsg ? 1 : 0);
  MsgProtocolScrubber protoFlow(ChannelID,
                                static_cast<PASSTHRU_MSG *>(pFlowControlMsg),
                                pFlowControlMsg ? 1 : 0);

  const long r = target.StartMsgFilter(ChannelID, FilterType, pMaskMsg, pPatternMsg,
                                       pFlowControlMsg, pMsgID);
  SHIM_TRACE("<<-- PassThruStartMsgFilter [ MsgId:%lu ] -> %s",
             pMsgID ? *pMsgID : 0UL, err2str(r));
  return r;
  SHIM_GUARD_END("PassThruStartMsgFilter")
}

long WINAPI PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruStopMsgFilter [ ChannelId:%lu MsgId:%lu ]", ChannelID,
             MsgID);
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruStopMsgFilter");
  if (const auto d2State = d2ChannelSnapshot(ChannelID)) {
    std::lock_guard<std::mutex> lock(d2State->mutex);
    const auto it = d2State->filters.find(MsgID);
    if (it == d2State->filters.end())
      return ERR_INVALID_FILTER_ID;
    d2State->filters.erase(it);
    return STATUS_NOERROR;
  }
  const long r = target.StopMsgFilter(ChannelID, MsgID);
  SHIM_TRACE("<<-- PassThruStopMsgFilter -> %s", err2str(r));
  return r;
  SHIM_GUARD_END("PassThruStopMsgFilter")
}

long WINAPI PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin,
                                          unsigned long Voltage) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruSetProgrammingVoltage [ DeviceId:%lu Pin:%lu Voltage:%lu ]",
             DeviceID, Pin, Voltage);
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruSetProgrammingVoltage");
  const long r = target.SetProgrammingVoltage(DeviceID, Pin, Voltage);
  SHIM_TRACE("<<-- PassThruSetProgrammingVoltage -> %s", err2str(r));
  return r;
  SHIM_GUARD_END("PassThruSetProgrammingVoltage")
}

long WINAPI PassThruReadVersion(unsigned long DeviceID, char *pFirmwareVersion,
                                char *pDllVersion, char *pApiVersion) {
  SHIM_GUARD_BEGIN
  SHIM_TRACE("-->> PassThruReadVersion [ DeviceId:%lu ]", DeviceID);
  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruReadVersion");
  const long r =
      target.ReadVersion(DeviceID, pFirmwareVersion, pDllVersion, pApiVersion);
  SHIM_TRACE("<<-- PassThruReadVersion [ FW:%s DLL:%s API:%s ] -> %s",
             pFirmwareVersion ? pFirmwareVersion : "", pDllVersion ? pDllVersion : "",
             pApiVersion ? pApiVersion : "", err2str(r));
  return r;
  SHIM_GUARD_END("PassThruReadVersion")
}

long WINAPI PassThruGetLastError(char *pErrorDescription) {
  SHIM_GUARD_BEGIN_NOCLEAR
  if (!pErrorDescription)
    return ERR_NULL_PARAMETER;

  // A pending shim-level message describes a call that never reached the
  // target, so it wins over whatever the target still remembers. It is *not*
  // consumed: J2534 defines this as "the last error", not a queue, and VB6
  // wrappers routinely read it twice - once to log, once to display.
  // setLastError clears it when the next call reaches the target.
  std::string pending;
  {
    Guard g;
    pending = g_lastError;
  }
  if (!pending.empty()) {
    // J2534 mandates an 80-byte buffer supplied by the caller.
    strncpy_s(pErrorDescription, 80, pending.c_str(), _TRUNCATE);
    return STATUS_NOERROR;
  }

  const Target target = targetSnapshot();
  if (target.loaded())
    return target.GetLastError_(pErrorDescription);
  strncpy_s(pErrorDescription, 80, "No error", _TRUNCATE);
  return STATUS_NOERROR;
  SHIM_GUARD_END("PassThruGetLastError")
}

long WINAPI PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                          void *pInput, void *pOutput) {
  SHIM_GUARD_BEGIN
  const char *name = ioctlName(IoctlID);
  const bool standardIoctl = isStandardIoctl(IoctlID);

  if (!standardIoctl) {
    const Policy p = policyFor(g_cfg, Category::ioctl, IoctlID, name, standardIoctl);
    if (p != Policy::forward) {
      SHIM_ERROR("ABSORB PassThruIoctl vendor id 0x%08lX (%s) - not forwarded",
                 IoctlID, name);
      return p == Policy::reject ? ERR_INVALID_IOCTL_ID : STATUS_NOERROR;
    }
    SHIM_ERROR("FORWARDING vendor ioctl 0x%08lX (%s) by explicit configuration",
               IoctlID, name);
  }

  if (IoctlID != SET_CONFIG && IoctlID != GET_CONFIG) {
    SHIM_TRACE("Calling PassThruIoctl %s [ ChannelId:%lu ]", name, ChannelID);
    const Target target = targetSnapshot();
    if (!target.loaded())
      return noTarget("PassThruIoctl");
    const auto d2State = IoctlID == CLEAR_RX_BUFFER
                             ? d2ChannelSnapshot(ChannelID)
                             : std::shared_ptr<d2::D2ChannelState>{};
    long r = target.Ioctl(ChannelID, IoctlID, pInput, pOutput);

    // MongoosePro JLR answers ERR_FAILED here; DiCE implements it, and SDA
    // calls it before every request.
    if (r != STATUS_NOERROR && IoctlID == CLEAR_RX_BUFFER &&
        g_cfg.emulateClearRxBuffer) {
      const long drained = drainRxQueue(target, ChannelID, r);
      if (drained == STATUS_NOERROR)
        SHIM_INFO("CLEAR_RX_BUFFER: target answered %s, emulated by draining the "
                  "queue instead",
                  err2str(r));
      r = drained;
    }

    if (IoctlID == CLEAR_RX_BUFFER && r == STATUS_NOERROR)
      clearD2RxState(d2State);

    SHIM_TRACE("<<-- PassThruIoctl %s -> %s", name, err2str(r));
    return r;
  }

  // SET_CONFIG / GET_CONFIG: split the parameter list, forward only the
  // standard part. GET_CONFIG is filtered for the same reason as SET_CONFIG -
  // a vendor parameter id means something else on the target device.
  SCONFIG_LIST *list = static_cast<SCONFIG_LIST *>(pInput);
  const auto d2State = d2ChannelSnapshot(ChannelID);
  bool d2StMinPresent = false;
  uint32_t d2StMinValue = 0;
  if (d2State && list && list->ConfigPtr) {
    std::lock_guard<std::mutex> lock(d2State->mutex);
    for (uint32_t i = 0; i < list->NumOfParams; ++i) {
      SCONFIG &param = list->ConfigPtr[i];
      if (param.Parameter != j2534::dice::CAN_XON_XOFF_STMIN)
        continue;
      d2StMinPresent = true;
      if (IoctlID == GET_CONFIG)
        param.Value = d2State->stMin;
      else
        d2StMinValue = param.Value;
    }
  }
  ConfigFilter f = filterConfigList(g_cfg, list);
  bool fabricateUpdatePresent = false;
  bool fabricateUpdateValue = false;

  for (const SCONFIG &p : f.absorbed) {
    SHIM_INFO("ABSORB PassThruIoctl %s/%s (0x%08X value %u) - not forwarded", name,
              configParamName(p.Parameter), p.Parameter, p.Value);
    if (IoctlID == SET_CONFIG &&
        p.Parameter == j2534::dice::ISO15765_SWDL_FABRICATE) {
      fabricateUpdatePresent = true;
      fabricateUpdateValue = p.Value != 0;
    }
  }
  for (const SCONFIG &p : f.forwarded)
    SHIM_TRACE("Calling PassThruIoctl %s/%s (value: %u)", name,
               configParamName(p.Parameter), p.Value);

  if (!f.rejected.empty()) {
    for (const SCONFIG &p : f.rejected)
      SHIM_ERROR("REJECT PassThruIoctl %s/%s (0x%08X) - the whole call fails", name,
                 configParamName(p.Parameter), p.Parameter);
    return ERR_INVALID_IOCTL_VALUE;
  }
  if (f.forwarded.empty()) {
    if (d2State && IoctlID == SET_CONFIG && d2StMinPresent) {
      std::lock_guard<std::mutex> lock(d2State->mutex);
      d2State->stMin = d2StMinValue;
      d2State->timing.setStMin(std::chrono::milliseconds(d2StMinValue));
      SHIM_TRACE("D2 STmin changed to %lu", static_cast<unsigned long>(d2StMinValue));
    }
    if (fabricateUpdatePresent)
      noteFabricate(ChannelID, fabricateUpdateValue);
    return STATUS_NOERROR; // everything was absorbed; nothing to do
  }

  const Target target = targetSnapshot();
  if (!target.loaded())
    return noTarget("PassThruIoctl");

  SCONFIG_LIST filtered;
  filtered.NumOfParams = static_cast<uint32_t>(f.forwarded.size());
  filtered.ConfigPtr = f.forwarded.data();

  const long r = target.Ioctl(ChannelID, IoctlID, &filtered, pOutput);

  if (r == STATUS_NOERROR && fabricateUpdatePresent)
    noteFabricate(ChannelID, fabricateUpdateValue);

  if (r == STATUS_NOERROR && d2State && IoctlID == SET_CONFIG && d2StMinPresent) {
    std::lock_guard<std::mutex> lock(d2State->mutex);
    d2State->stMin = d2StMinValue;
    d2State->timing.setStMin(std::chrono::milliseconds(d2StMinValue));
    SHIM_TRACE("D2 STmin changed to %lu", static_cast<unsigned long>(d2StMinValue));
  }

  // GET_CONFIG writes the answers into the list it was given; copy them back
  // into the caller's list, leaving absorbed entries untouched.
  if (IoctlID == GET_CONFIG && r == STATUS_NOERROR && list && list->ConfigPtr) {
    size_t next = 0;
    for (uint32_t i = 0; i < list->NumOfParams && next < f.forwarded.size(); ++i) {
      if (list->ConfigPtr[i].Parameter == f.forwarded[next].Parameter)
        list->ConfigPtr[i].Value = f.forwarded[next++].Value;
    }
  }

  SHIM_TRACE("<<-- PassThruIoctl %s -> %s", name, err2str(r));
  return r;
  SHIM_GUARD_END("PassThruIoctl")
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    g_self = hModule;
    DisableThreadLibraryCalls(hModule);
    break;
  case DLL_PROCESS_DETACH:
    // Do not touch the target library here: the loader may already have
    // unloaded it. Mark the lock unusable before deleting it, then stop
    // logging.
    if (InterlockedExchange(&g_lockReady, 0) == 1)
      DeleteCriticalSection(&g_lock);
    Log::instance().close();
    break;
  default:
    break;
  }
  return TRUE;
}
