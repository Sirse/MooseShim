#include "doctest.h"

#include "../shim/d2/d2_transport.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <vector>

using namespace shim::d2;

namespace {

D2CanFrame frame(uint32_t canId, std::initializer_list<uint8_t> bytes) {
  D2CanFrame result;
  result.canId = canId;
  result.data.fill(0);
  result.dataSize = static_cast<uint8_t>(bytes.size());
  std::copy(bytes.begin(), bytes.end(), result.data.begin());
  return result;
}

} // namespace

TEST_CASE("D2 SF uses the VolvoTools byte layout") {
  const D2LogicalMessage message{0xFFFFE, {0x40, 0xBE, 0x68, 0x25,
                                           0x09, 0x85, 0x31}};
  std::vector<D2CanFrame> frames;
  REQUIRE(D2Segmenter::encode(message, &frames));
  REQUIRE(frames.size() == 1);
  CHECK(frames[0].canId == 0xFFFFE);
  CHECK(frames[0].data ==
        CanPayload{0xCF, 0x40, 0xBE, 0x68, 0x25, 0x09, 0x85, 0x31});
}

TEST_CASE("D2 FF and LF preserve a payload crossing the SF boundary") {
  const D2LogicalMessage message{0xFFFFE, {0x40, 0xBE, 0x68, 0x25,
                                           0x09, 0x85, 0x31, 0x03}};
  std::vector<D2CanFrame> frames;
  REQUIRE(D2Segmenter::encode(message, &frames));
  REQUIRE(frames.size() == 2);
  CHECK(frames[0].data ==
        CanPayload{0x8F, 0x40, 0xBE, 0x68, 0x25, 0x09, 0x85, 0x31});
  CHECK(frames[1].data == CanPayload{0x49, 0x03, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("D2 IF sequence follows the VolvoTools wraparound") {
  D2LogicalMessage message;
  message.canId = 0xFFFFE;
  message.payload.resize(64);
  for (size_t i = 0; i < message.payload.size(); ++i)
    message.payload[i] = static_cast<uint8_t>(i);

  std::vector<D2CanFrame> frames;
  REQUIRE(D2Segmenter::encode(message, &frames));
  REQUIRE(frames.size() == 10);
  CHECK(frames[0].data[0] == 0x8F);
  CHECK(frames[1].data[0] == 0x0A);
  CHECK(frames[2].data[0] == 0x0B);
  CHECK(frames[3].data[0] == 0x0C);
  CHECK(frames[4].data[0] == 0x0D);
  CHECK(frames[5].data[0] == 0x0E);
  CHECK(frames[6].data[0] == 0x0F);
  CHECK(frames[7].data[0] == 0x08);
  CHECK(frames[8].data[0] == 0x09);
  CHECK(frames[9].data[0] == 0x49);
}

TEST_CASE("D2 reassembly returns only a complete logical message") {
  D2Reassembler reassembler;
  const auto start = std::chrono::steady_clock::time_point{};
  CHECK(reassembler.push(
            frame(0xFFFFE, {0x8F, 0x40, 0xBE, 0x68, 0x25, 0x09, 0x85, 0x31}),
            start)
            .result == D2FeedResult::incomplete);
  CHECK(reassembler.push(frame(0xFFFFE, {0x49, 0x03}), start).result ==
        D2FeedResult::complete);
  const D2Feed result = reassembler.push(
      frame(0x00003, {0xCA, 0x40, 0xB9}), start);
  CHECK(result.result == D2FeedResult::complete);
  CHECK(result.type == D2FrameType::single);
  CHECK(result.message.canId == 0x00003);
  CHECK(result.message.payload == std::vector<uint8_t>{0x40, 0xB9});
}

TEST_CASE("D2 assemblies are independent per CAN id") {
  D2Reassembler first;
  D2Reassembler second;
  const auto start = std::chrono::steady_clock::time_point{};

  CHECK(first.push(frame(0x100, {0x8F, 1, 2, 3, 4, 5, 6, 7}), start).result ==
        D2FeedResult::incomplete);
  CHECK(second.push(frame(0x200, {0x8F, 8, 9, 10, 11, 12, 13, 14}), start).result ==
        D2FeedResult::incomplete);
  CHECK(first.push(frame(0x100, {0x49, 15}), start).result ==
        D2FeedResult::complete);
  const D2Feed result = second.push(frame(0x200, {0x49, 16}), start);
  CHECK(result.result == D2FeedResult::complete);
  CHECK(result.message.canId == 0x200);
  CHECK(result.message.payload == std::vector<uint8_t>{8, 9, 10, 11, 12, 13, 14, 16});
}

TEST_CASE("raw D2 frames remain available for Volvo bootloader traffic") {
  D2Reassembler reassembler;
  const D2Feed result = reassembler.push(
      frame(0x00003, {0x40, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
      std::chrono::steady_clock::now(), true);
  CHECK(result.result == D2FeedResult::complete);
  CHECK(result.type == D2FrameType::raw);
  CHECK(result.message.payload ==
        std::vector<uint8_t>{0x40, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

TEST_CASE("raw D2 frames honor their actual CAN payload length") {
  D2Reassembler reassembler;
  const D2Feed result = reassembler.push(
      frame(0x00003, {0x40, 0xB9}), std::chrono::steady_clock::now(), true);
  CHECK(result.result == D2FeedResult::complete);
  CHECK(result.type == D2FrameType::raw);
  CHECK(result.message.payload == std::vector<uint8_t>{0x40, 0xB9});
}

TEST_CASE("D2 TX indications accept ID-only and full echoes") {
  const D2CanFrame sent = frame(0x123, {0xCF, 0x40, 0xBE, 0x68, 0x25, 0x09,
                                       0x85, 0x31});

  D2PendingRx idOnly;
  idOnly.sourceDataSize = 4;
  idOnly.frame.canId = sent.canId;
  idOnly.frame.dataSize = 0;
  CHECK(matchesTxIndication(idOnly, sent));

  D2PendingRx fullEcho;
  fullEcho.sourceDataSize = 12;
  fullEcho.frame = sent;
  CHECK(matchesTxIndication(fullEcho, sent));

  fullEcho.frame.data[7] ^= 0x01;
  CHECK_FALSE(matchesTxIndication(fullEcho, sent));
}

TEST_CASE("D2 wrong sequence clears only the affected assembly") {
  D2Reassembler reassembler;
  const auto start = std::chrono::steady_clock::time_point{};
  REQUIRE(reassembler.push(frame(0xFFFFE, {0x8F, 1, 2, 3, 4, 5, 6, 7}), start)
              .result == D2FeedResult::incomplete);
  const D2Feed wrong = reassembler.push(frame(0xFFFFE, {0x0B, 8, 9, 10, 11, 12, 13, 14}),
                                         start);
  CHECK(wrong.result == D2FeedResult::wrong_sequence);
  CHECK_FALSE(reassembler.active());
  CHECK(reassembler.push(frame(0xFFFFE, {0xCF, 0x40, 0xBE, 0x68, 0x25, 0x09, 0x85, 0x31}),
                         start)
                 .result == D2FeedResult::complete);
}

TEST_CASE("D2 timeout uses a monotonic deadline") {
  D2Reassembler reassembler(4095, std::chrono::milliseconds(10));
  const auto start = std::chrono::steady_clock::time_point{};
  REQUIRE(reassembler.push(frame(0xFFFFE, {0x8F, 1, 2, 3, 4, 5, 6, 7}), start)
              .result == D2FeedResult::incomplete);
  const D2Feed timeout = reassembler.expire(start + std::chrono::milliseconds(11));
  CHECK(timeout.result == D2FeedResult::timeout);
  CHECK_FALSE(reassembler.active());
}

TEST_CASE("D2 rejects malformed logical messages") {
  D2LogicalMessage empty{0xFFFFE, {}};
  std::vector<D2CanFrame> frames;
  CHECK_FALSE(D2Segmenter::encode(empty, &frames));

  D2LogicalMessage badId{0x20000000, {1}};
  CHECK_FALSE(D2Segmenter::encode(badId, &frames));

  D2LogicalMessage oversized{0xFFFFE, std::vector<uint8_t>(4096, 0)};
  CHECK_FALSE(D2Segmenter::encode(oversized, &frames));

  D2Reassembler reassembler;
  CHECK(reassembler.push(frame(0xFFFFE, {0xCF, 0x40})).result ==
        D2FeedResult::invalid);
}

TEST_CASE("D2 reassembly accepts the 4095-byte maximum") {
  D2LogicalMessage message;
  message.canId = 0xFFFFE;
  message.payload.resize(kDefaultMaxPayloadSize);
  for (size_t i = 0; i < message.payload.size(); ++i)
    message.payload[i] = static_cast<uint8_t>(i & 0xFF);
  std::vector<D2CanFrame> frames;
  REQUIRE(D2Segmenter::encode(message, &frames));
  D2Reassembler reassembler;
  D2Feed result;
  for (const auto &wire : frames)
    result = reassembler.push(wire);
  CHECK(result.result == D2FeedResult::complete);
  CHECK(result.message.payload == message.payload);
}

TEST_CASE("D2 STmin is represented without a wall clock") {
  D2Timing timing;
  const auto start = std::chrono::steady_clock::time_point{};
  timing.setStMin(std::chrono::microseconds(1500));
  timing.markFrameSent(start);
  CHECK(timing.stMin() == std::chrono::microseconds(1500));
  CHECK(timing.nextFrameTime() == start + std::chrono::microseconds(1500));
}
