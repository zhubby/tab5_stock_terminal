#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tab5::longbridge {

enum class WireCodec : std::uint8_t {
    Protobuf = 1,
};

enum class PlatformType : std::uint8_t {
    Terminal = 8,
    OpenApi = 9,
};

enum class PacketType : std::uint8_t {
    Request = 1,
    Response = 2,
    Push = 3,
};

enum class CommandIntent : std::uint16_t {
    Heartbeat = 1,
    Auth = 2,
    QuoteSubscribe = 6,
    QuoteUnsubscribe = 7,
    QuotePull = 11,
    QuotePush = 101,
};

struct WireFrame {
    WireCodec codec { WireCodec::Protobuf };
    PacketType packet_type { PacketType::Request };
    CommandIntent command { CommandIntent::Auth };
    std::uint16_t flags { 0 };
    std::uint16_t timeout_ms { 5000 };
    std::uint8_t status_code { 0 };
    std::uint32_t request_id { 0 };
    std::vector<std::uint8_t> payload;
};

enum class DecodeStatus {
    Ok,
    Incomplete,
    BadMagic,
    UnsupportedVersion,
    PayloadTooLarge,
};

struct DecodeResult {
    DecodeStatus status { DecodeStatus::Incomplete };
    std::optional<WireFrame> frame;
    std::size_t consumed { 0 };
};

constexpr std::size_t kHandshakeLength = 2;
constexpr std::size_t kRequestHeaderLength = 11;
constexpr std::size_t kResponseHeaderLength = 10;
constexpr std::size_t kPushHeaderLength = 5;
constexpr std::size_t kMaxFramePayloadLength = 256 * 1024;

std::vector<std::uint8_t> encode_handshake(WireCodec codec = WireCodec::Protobuf,
                                           PlatformType platform = PlatformType::OpenApi);
std::vector<std::uint8_t> encode_frame(const WireFrame& frame);
DecodeResult decode_frame(const std::uint8_t* data,
                          std::size_t length,
                          std::size_t max_payload_length = kMaxFramePayloadLength);
std::string decode_status_text(DecodeStatus status);

class RequestIdGenerator {
public:
    std::uint32_t next();

private:
    std::uint32_t current_ { 0 };
};

class BackoffPolicy {
public:
    explicit BackoffPolicy(std::uint32_t base_ms = 500,
                           std::uint32_t max_ms = 30'000,
                           std::uint8_t factor = 2);

    std::uint32_t next_delay_ms();
    void reset();

private:
    std::uint32_t base_ms_;
    std::uint32_t max_ms_;
    std::uint8_t factor_;
    std::uint32_t current_ms_ { 0 };
};

} // namespace tab5::longbridge
