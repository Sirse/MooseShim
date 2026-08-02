#include "d2_transport.h"

#include <algorithm>

namespace shim::d2 {

namespace {

constexpr uint8_t kSingleBase = 0xC8;
constexpr uint8_t kFirstBase = 0x88;
constexpr uint8_t kIntermediateBase = 0x08;
constexpr uint8_t kLastBase = 0x48;

bool inRange(uint8_t value, uint8_t base) {
  return value >= base && value <= static_cast<uint8_t>(base + 7);
}

uint8_t frameLength(uint8_t header, uint8_t base) {
  return static_cast<uint8_t>(header - base);
}

void setError(std::string *error, const char *message) {
  if (error)
    *error = message;
}

} // namespace

bool D2Segmenter::encode(const D2LogicalMessage &message,
                         std::vector<D2CanFrame> *frames,
                         std::string *error) {
  if (!frames) {
    setError(error, "output frame vector is null");
    return false;
  }
  frames->clear();
  if (message.canId > kMaxCanId) {
    setError(error, "CAN id exceeds the 29-bit range");
    return false;
  }
  if (message.payload.empty()) {
    setError(error, "D2 payload is empty");
    return false;
  }
  if (message.payload.size() > kDefaultMaxPayloadSize) {
    setError(error, "D2 payload exceeds the supported size");
    return false;
  }

  const bool multiple = message.payload.size() > kD2SegmentPayloadSize;
  size_t offset = 0;
  // VolvoTools advances the series counter for the first frame as well; the
  // first intermediate frame therefore carries 0x0A.
  uint8_t sequence = 0x09;
  while (offset < message.payload.size()) {
    const size_t remaining = message.payload.size() - offset;
    const size_t size = std::min(remaining, kD2SegmentPayloadSize);
    D2CanFrame frame;
    frame.canId = message.canId;
    frame.data.fill(0);
    frame.dataSize = static_cast<uint8_t>(kCanPayloadSize);

    uint8_t header = 0;
    if (!multiple) {
      header = static_cast<uint8_t>(kSingleBase + size);
    } else if (offset == 0) {
      header = static_cast<uint8_t>(kFirstBase + size);
    } else if (remaining <= kD2SegmentPayloadSize) {
      header = static_cast<uint8_t>(kLastBase + size);
    } else {
      sequence = static_cast<uint8_t>((sequence - 8u + 1u) % 8u + 8u);
      header = sequence;
    }

    frame.data[0] = header;
    std::copy_n(message.payload.data() + offset, size, frame.data.data() + 1);
    frames->push_back(frame);
    offset += size;
  }
  return true;
}

D2Reassembler::D2Reassembler(size_t maxPayloadSize,
                             std::chrono::milliseconds assemblyTimeout)
    : maxPayloadSize_(maxPayloadSize), assemblyTimeout_(assemblyTimeout) {}

bool matchesTxIndication(const D2PendingRx &indication,
                         const D2CanFrame &sent) {
  if (indication.frame.canId != sent.canId)
    return false;
  if (indication.sourceDataSize == 4 && indication.frame.dataSize == 0)
    return true;
  if (indication.sourceDataSize != 4UL + sent.dataSize ||
      indication.frame.dataSize != sent.dataSize)
    return false;
  return std::equal(indication.frame.data.begin(),
                    indication.frame.data.begin() + indication.frame.dataSize,
                    sent.data.begin());
}

bool isTransportStartFrame(const D2CanFrame &frame) {
  if (frame.dataSize == 0)
    return false;
  return inRange(frame.data[0], kSingleBase) ||
         inRange(frame.data[0], kFirstBase);
}

bool isTransportFrame(const D2CanFrame &frame) {
  if (frame.dataSize == 0)
    return false;
  return isTransportStartFrame(frame) ||
         inRange(frame.data[0], kIntermediateBase) ||
         inRange(frame.data[0], kLastBase);
}

D2Feed D2Reassembler::fail(D2FeedResult result, D2FrameType type,
                            const char *message) {
  reset();
  D2Feed feed;
  feed.result = result;
  feed.type = type;
  feed.error = message;
  return feed;
}

D2Feed D2Reassembler::append(uint32_t canId, const uint8_t *data, size_t size,
                              D2FrameType type) {
  if (size == 0 || size > kD2SegmentPayloadSize)
    return fail(D2FeedResult::invalid, type, "invalid D2 segment length");
  if (size > maxPayloadSize_ || payload_.size() > maxPayloadSize_ - size)
    return fail(D2FeedResult::oversized, type, "D2 assembly exceeds the limit");

  payload_.insert(payload_.end(), data, data + size);
  D2Feed feed;
  feed.result = D2FeedResult::incomplete;
  feed.type = type;
  feed.message.canId = canId;
  return feed;
}

D2Feed D2Reassembler::push(const D2CanFrame &frame,
                            std::chrono::steady_clock::time_point now,
                            bool forceRaw) {
  if (frame.canId > kMaxCanId)
    return fail(D2FeedResult::invalid, D2FrameType::raw,
                "CAN id exceeds the 29-bit range");

  if (active_ && now - lastFrame_ > assemblyTimeout_)
    reset();

  if (forceRaw) {
    if (active_)
      return fail(D2FeedResult::invalid, D2FrameType::raw,
                  "raw D2 frame interrupted an active assembly");
    if (frame.dataSize == 0 || frame.dataSize > kCanPayloadSize)
      return fail(D2FeedResult::invalid, D2FrameType::raw,
                  "invalid raw D2 frame length");
    D2Feed feed;
    feed.result = D2FeedResult::complete;
    feed.type = D2FrameType::raw;
    feed.message.canId = frame.canId;
    feed.message.payload.assign(frame.data.begin(),
                                frame.data.begin() + frame.dataSize);
    return feed;
  }

  const uint8_t header = frame.data[0];
  if (inRange(header, kSingleBase)) {
    if (active_)
      return fail(D2FeedResult::invalid, D2FrameType::single,
                  "D2 single frame interrupted an active assembly");
    const size_t size = frameLength(header, kSingleBase);
    if (size == 0)
      return fail(D2FeedResult::invalid, D2FrameType::single,
                  "empty D2 single frame");
    if (frame.dataSize < size + 1)
      return fail(D2FeedResult::invalid, D2FrameType::single,
                  "D2 single frame is shorter than its header");
    D2Feed feed;
    feed.result = D2FeedResult::complete;
    feed.type = D2FrameType::single;
    feed.message.canId = frame.canId;
    feed.message.payload.assign(frame.data.data() + 1,
                                frame.data.data() + 1 + size);
    return feed;
  }

  if (inRange(header, kFirstBase)) {
    if (active_)
      return fail(D2FeedResult::invalid, D2FrameType::first,
                  "new D2 first frame interrupted an active assembly");
    const size_t size = frameLength(header, kFirstBase);
    if (size == 0)
      return fail(D2FeedResult::invalid, D2FrameType::first,
                  "empty D2 first frame");
    if (frame.dataSize < size + 1)
      return fail(D2FeedResult::invalid, D2FrameType::first,
                  "D2 first frame is shorter than its header");
    active_ = true;
    canId_ = frame.canId;
    expectedSequence_ = 0x0A;
    lastSequence_ = 0;
    payload_.clear();
    const D2Feed feed = append(frame.canId, frame.data.data() + 1, size,
                               D2FrameType::first);
    lastFrame_ = now;
    return feed;
  }

  if (inRange(header, kIntermediateBase)) {
    if (!active_)
      return fail(D2FeedResult::invalid, D2FrameType::intermediate,
                  "D2 intermediate frame without a first frame");
    if (frame.canId != canId_)
      return D2Feed{D2FeedResult::ignored, D2FrameType::intermediate, {}, {}};
    if (frame.dataSize < kD2SegmentPayloadSize + 1)
      return fail(D2FeedResult::invalid, D2FrameType::intermediate,
                  "D2 intermediate frame is shorter than its payload");
    if (header == lastSequence_)
      return fail(D2FeedResult::duplicate, D2FrameType::intermediate,
                  "duplicate D2 intermediate frame");
    if (header != expectedSequence_)
      return fail(D2FeedResult::wrong_sequence, D2FrameType::intermediate,
                  "unexpected D2 intermediate sequence");
    const D2Feed feed = append(frame.canId, frame.data.data() + 1,
                               kD2SegmentPayloadSize,
                               D2FrameType::intermediate);
    lastSequence_ = header;
    expectedSequence_ = static_cast<uint8_t>((header - 8u + 1u) % 8u + 8u);
    lastFrame_ = now;
    return feed;
  }

  if (inRange(header, kLastBase)) {
    if (!active_)
      return fail(D2FeedResult::invalid, D2FrameType::last,
                  "D2 last frame without a first frame");
    if (frame.canId != canId_)
      return D2Feed{D2FeedResult::ignored, D2FrameType::last, {}, {}};
    const size_t size = frameLength(header, kLastBase);
    if (frame.dataSize < size + 1)
      return fail(D2FeedResult::invalid, D2FrameType::last,
                  "D2 last frame is shorter than its header");
    const D2Feed tail = append(frame.canId, frame.data.data() + 1, size,
                               D2FrameType::last);
    if (tail.result != D2FeedResult::incomplete)
      return tail;
    D2Feed feed;
    feed.result = D2FeedResult::complete;
    feed.type = D2FrameType::last;
    feed.message.canId = canId_;
    feed.message.payload = std::move(payload_);
    reset();
    return feed;
  }

  // VolvoTools also sends raw D2 frames for bootloader requests and responses.
  // They carry the logical payload directly and use the full CAN data field.
  if (!forceRaw)
    return D2Feed{D2FeedResult::ignored, D2FrameType::raw, {}, {}};
  if (active_)
    return fail(D2FeedResult::invalid, D2FrameType::raw,
                "raw D2 frame interrupted an active assembly");
  if (frame.dataSize == 0 || frame.dataSize > kCanPayloadSize)
    return fail(D2FeedResult::invalid, D2FrameType::raw,
                "invalid raw D2 frame length");
  D2Feed feed;
  feed.result = D2FeedResult::complete;
  feed.type = D2FrameType::raw;
  feed.message.canId = frame.canId;
  feed.message.payload.assign(frame.data.begin(),
                              frame.data.begin() + frame.dataSize);
  return feed;
}

D2Feed D2Reassembler::expire(std::chrono::steady_clock::time_point now) {
  if (!active_ || now - lastFrame_ <= assemblyTimeout_)
    return {};
  return fail(D2FeedResult::timeout, D2FrameType::intermediate,
              "D2 assembly timed out");
}

void D2Reassembler::reset() {
  active_ = false;
  canId_ = 0;
  expectedSequence_ = 0x0A;
  lastSequence_ = 0;
  payload_.clear();
  lastFrame_ = {};
}

bool D2Reassembler::active() const { return active_; }

std::chrono::steady_clock::time_point D2Reassembler::deadline() const {
  return lastFrame_ + assemblyTimeout_;
}

void D2Timing::setStMin(std::chrono::microseconds value) { stMin_ = value; }

std::chrono::microseconds D2Timing::stMin() const { return stMin_; }

void D2Timing::markFrameSent(std::chrono::steady_clock::time_point now) {
  lastFrame_ = now;
}

std::chrono::steady_clock::time_point D2Timing::nextFrameTime() const {
  return lastFrame_ + stMin_;
}

} // namespace shim::d2
