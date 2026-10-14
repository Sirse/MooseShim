#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <mutex>
#include <map>
#include <vector>

namespace shim::d2 {

constexpr uint32_t kMaxCanId = 0x1FFFFFFFu;
constexpr uint32_t kDefaultD2CanId = 0xFFFFEu;
constexpr size_t kCanPayloadSize = 8;
constexpr size_t kD2SegmentPayloadSize = 7;
constexpr size_t kDefaultMaxPayloadSize = 4095;

using CanPayload = std::array<uint8_t, kCanPayloadSize>;

struct D2CanFrame {
  uint32_t canId = 0;
  CanPayload data{};
  uint8_t dataSize = static_cast<uint8_t>(kCanPayloadSize);
};

struct D2LogicalMessage {
  uint32_t canId = 0;
  std::vector<uint8_t> payload;
};

struct D2PendingRx {
  D2CanFrame frame;
  unsigned long sourceDataSize = 0;
  unsigned long protocol = 0;
  unsigned long rxStatus = 0;
  unsigned long txFlags = 0;
  unsigned long timestamp = 0;
  unsigned long extraDataIndex = 0;
};

bool matchesTxIndication(const D2PendingRx &indication,
                         const D2CanFrame &sent);

bool isTransportStartFrame(const D2CanFrame &frame);

bool isTransportFrame(const D2CanFrame &frame);

enum class D2FrameType {
  single,
  first,
  intermediate,
  last,
  raw,
};

enum class D2FeedResult {
  ignored,
  incomplete,
  complete,
  invalid,
  wrong_sequence,
  duplicate,
  oversized,
  timeout,
};

struct D2Feed {
  D2FeedResult result = D2FeedResult::ignored;
  D2FrameType type = D2FrameType::raw;
  D2LogicalMessage message;
  std::string error;
};

class D2Segmenter {
public:
  static bool encode(const D2LogicalMessage &message,
                     std::vector<D2CanFrame> *frames,
                     std::string *error = nullptr);
};

class D2Reassembler {
public:
  explicit D2Reassembler(
      size_t maxPayloadSize = kDefaultMaxPayloadSize,
      std::chrono::milliseconds assemblyTimeout = std::chrono::milliseconds(1000));

  D2Feed push(const D2CanFrame &frame,
              std::chrono::steady_clock::time_point now =
                  std::chrono::steady_clock::now(), bool forceRaw = false);
  D2Feed expire(std::chrono::steady_clock::time_point now =
                    std::chrono::steady_clock::now());
  void reset();
  bool active() const;
  std::chrono::steady_clock::time_point deadline() const;

private:
  D2Feed fail(D2FeedResult result, D2FrameType type, const char *message);
  D2Feed append(uint32_t canId, const uint8_t *data, size_t size,
                D2FrameType type);

  size_t maxPayloadSize_;
  std::chrono::milliseconds assemblyTimeout_;
  bool active_ = false;
  uint32_t canId_ = 0;
  uint8_t expectedSequence_ = 0x0A;
  uint8_t lastSequence_ = 0;
  std::vector<uint8_t> payload_;
  std::chrono::steady_clock::time_point lastFrame_{};
};

class D2Timing {
public:
  void setStMin(std::chrono::microseconds value);
  std::chrono::microseconds stMin() const;
  void markFrameSent(std::chrono::steady_clock::time_point now);
  std::chrono::steady_clock::time_point nextFrameTime() const;

private:
  std::chrono::microseconds stMin_{0};
  std::chrono::steady_clock::time_point lastFrame_{};
};

struct D2ChannelState {
  unsigned long deviceId = 0;
  unsigned long channelId = 0;
  unsigned long hostProtocol = 0;
  unsigned long targetProtocol = 0;
  unsigned long baudRate = 0;
  unsigned long hostFlags = 0;
  unsigned long targetFlags = 0;
  unsigned long targetReceiveFilterId = 0;
  unsigned long pins = 0;
  unsigned long samplePoint = 0;
  uint32_t stMin = 0;
  bool ignoreIntermediateTxAck = true;
  D2Timing timing;
  std::map<uint32_t, D2Reassembler> reassemblers;
  std::deque<D2PendingRx> pendingRx;
  struct RawExpectation {
    uint32_t responseCanId = 0;
    std::chrono::steady_clock::time_point expiresAt{};
  };
  std::deque<RawExpectation> rawExpectations;
  struct Filter {
    unsigned long type = 1; // J2534 PASS_FILTER; kept numeric in the pure layer.
    uint32_t mask = 0;
    uint32_t pattern = 0;
  };
  std::map<unsigned long, Filter> filters;
  unsigned long nextFilterId = 1;
  struct DeferredError {
    long status = 0;
    std::string message;
  };
  std::deque<DeferredError> deferredErrors;
  // Host readers are serialized; target reads run in short slices under mutex.
  // This also serializes the TX-ack reader without starving periodic writes.
  mutable std::mutex readMutex;
  mutable std::mutex mutex;
};

} // namespace shim::d2
