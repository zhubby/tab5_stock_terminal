#include "longbridge/protocol.hpp"

#include <algorithm>

namespace tab5::longbridge {
namespace {

constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kHeaderTypeMask = 0x0F;
constexpr std::uint8_t kVerifyMask = 0x10;
constexpr std::uint8_t kGzipMask = 0x20;

void write_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void write_u24(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::uint16_t read_u16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U)
                                      | static_cast<std::uint16_t>(data[1]));
}

std::uint32_t read_u32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24U)
        | (static_cast<std::uint32_t>(data[1]) << 16U)
        | (static_cast<std::uint32_t>(data[2]) << 8U)
        | static_cast<std::uint32_t>(data[3]);
}

std::uint32_t read_u24(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 16U)
        | (static_cast<std::uint32_t>(data[1]) << 8U)
        | static_cast<std::uint32_t>(data[2]);
}

std::uint8_t header_flags(const WireFrame& frame)
{
    return static_cast<std::uint8_t>(frame.packet_type) | static_cast<std::uint8_t>(frame.flags & 0xF0U);
}

std::size_t header_length_for(PacketType packet_type)
{
    switch (packet_type) {
    case PacketType::Request:
        return kRequestHeaderLength;
    case PacketType::Response:
        return kResponseHeaderLength;
    case PacketType::Push:
        return kPushHeaderLength;
    }
    return 0;
}

} // namespace

std::vector<std::uint8_t> encode_handshake(WireCodec codec, PlatformType platform)
{
    return {
        static_cast<std::uint8_t>(kVersion | (static_cast<std::uint8_t>(codec) << 4U)),
        static_cast<std::uint8_t>(platform),
    };
}

std::vector<std::uint8_t> encode_frame(const WireFrame& frame)
{
    std::vector<std::uint8_t> out;
    out.reserve(header_length_for(frame.packet_type) + frame.payload.size());
    out.push_back(header_flags(frame));
    out.push_back(static_cast<std::uint8_t>(frame.command));

    switch (frame.packet_type) {
    case PacketType::Request:
        write_u32(out, frame.request_id);
        write_u16(out, frame.timeout_ms);
        write_u24(out, static_cast<std::uint32_t>(frame.payload.size()));
        break;
    case PacketType::Response:
        write_u32(out, frame.request_id);
        out.push_back(frame.status_code);
        write_u24(out, static_cast<std::uint32_t>(frame.payload.size()));
        break;
    case PacketType::Push:
        write_u24(out, static_cast<std::uint32_t>(frame.payload.size()));
        break;
    }

    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

DecodeResult decode_frame(const std::uint8_t* data,
                          std::size_t length,
                          std::size_t max_payload_length)
{
    DecodeResult result;

    if (length < 1) {
        result.status = DecodeStatus::Incomplete;
        return result;
    }

    const auto packet_type = static_cast<PacketType>(data[0] & kHeaderTypeMask);
    const std::size_t header_length = header_length_for(packet_type);
    if (header_length == 0) {
        result.status = DecodeStatus::BadMagic;
        return result;
    }
    if (length < header_length) {
        result.status = DecodeStatus::Incomplete;
        return result;
    }
    if ((data[0] & (kVerifyMask | kGzipMask)) != 0) {
        result.status = DecodeStatus::UnsupportedVersion;
        return result;
    }

    std::size_t cursor = 1;
    WireFrame frame;
    frame.packet_type = packet_type;
    frame.command = static_cast<CommandIntent>(data[cursor++]);

    std::uint32_t payload_length = 0;
    if (packet_type == PacketType::Request) {
        frame.request_id = read_u32(data + cursor);
        cursor += 4;
        frame.timeout_ms = read_u16(data + cursor);
        cursor += 2;
        payload_length = read_u24(data + cursor);
        cursor += 3;
    } else if (packet_type == PacketType::Response) {
        frame.request_id = read_u32(data + cursor);
        cursor += 4;
        frame.status_code = data[cursor++];
        payload_length = read_u24(data + cursor);
        cursor += 3;
    } else {
        payload_length = read_u24(data + cursor);
        cursor += 3;
    }

    if (payload_length > max_payload_length) {
        result.status = DecodeStatus::PayloadTooLarge;
        return result;
    }

    const std::size_t total_length = header_length + payload_length;
    if (length < total_length) {
        result.status = DecodeStatus::Incomplete;
        return result;
    }

    frame.flags = data[0] & 0xF0U;
    frame.payload.assign(data + header_length, data + total_length);

    result.status = DecodeStatus::Ok;
    result.consumed = total_length;
    result.frame = std::move(frame);
    return result;
}

std::string decode_status_text(DecodeStatus status)
{
    switch (status) {
    case DecodeStatus::Ok:
        return "ok";
    case DecodeStatus::Incomplete:
        return "incomplete";
    case DecodeStatus::BadMagic:
        return "bad magic";
    case DecodeStatus::UnsupportedVersion:
        return "unsupported version";
    case DecodeStatus::PayloadTooLarge:
        return "payload too large";
    }
    return "unknown";
}

std::uint32_t RequestIdGenerator::next()
{
    ++current_;
    if (current_ == 0) {
        ++current_;
    }
    return current_;
}

BackoffPolicy::BackoffPolicy(std::uint32_t base_ms, std::uint32_t max_ms, std::uint8_t factor)
    : base_ms_(base_ms)
    , max_ms_(std::max(base_ms, max_ms))
    , factor_(std::max<std::uint8_t>(factor, 1))
{
}

std::uint32_t BackoffPolicy::next_delay_ms()
{
    if (current_ms_ == 0) {
        current_ms_ = base_ms_;
    } else {
        const std::uint64_t next = static_cast<std::uint64_t>(current_ms_) * factor_;
        current_ms_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(next, max_ms_));
    }
    return current_ms_;
}

void BackoffPolicy::reset()
{
    current_ms_ = 0;
}

} // namespace tab5::longbridge
