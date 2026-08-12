#include "longbridge/quote_codec.hpp"

#include <cstdlib>
#include <utility>

namespace tab5::longbridge {
namespace {

constexpr std::uint32_t kWireTypeVarint = 0;
constexpr std::uint32_t kWireTypeLengthDelimited = 2;
constexpr std::uint32_t kSubTypeQuote = 1;

void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    while (value >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void write_key(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint32_t wire_type)
{
    write_varint(out, (field << 3U) | wire_type);
}

void write_string(std::vector<std::uint8_t>& out, std::uint32_t field, const std::string& value)
{
    write_key(out, field, kWireTypeLengthDelimited);
    write_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void write_bool(std::vector<std::uint8_t>& out, std::uint32_t field, bool value)
{
    write_key(out, field, kWireTypeVarint);
    write_varint(out, value ? 1 : 0);
}

void write_enum(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint32_t value)
{
    write_key(out, field, kWireTypeVarint);
    write_varint(out, value);
}

bool read_varint(const std::uint8_t* data,
                 std::size_t length,
                 std::size_t& cursor,
                 std::uint64_t& value_out)
{
    value_out = 0;
    std::uint32_t shift = 0;
    while (cursor < length && shift < 64) {
        const std::uint8_t byte = data[cursor++];
        value_out |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}

bool read_length_delimited(const std::uint8_t* data,
                           std::size_t length,
                           std::size_t& cursor,
                           std::string& value_out)
{
    std::uint64_t value_length = 0;
    if (!read_varint(data, length, cursor, value_length)) {
        return false;
    }
    if (value_length > length - cursor) {
        return false;
    }
    value_out.assign(reinterpret_cast<const char*>(data + cursor), static_cast<std::size_t>(value_length));
    cursor += static_cast<std::size_t>(value_length);
    return true;
}

bool read_length_delimited_bytes(const std::uint8_t* data,
                                 std::size_t length,
                                 std::size_t& cursor,
                                 std::vector<std::uint8_t>& value_out)
{
    std::uint64_t value_length = 0;
    if (!read_varint(data, length, cursor, value_length)) {
        return false;
    }
    if (value_length > length - cursor) {
        return false;
    }
    value_out.assign(data + cursor, data + cursor + static_cast<std::size_t>(value_length));
    cursor += static_cast<std::size_t>(value_length);
    return true;
}

bool skip_field(const std::uint8_t* data, std::size_t length, std::size_t& cursor, std::uint32_t wire_type)
{
    if (wire_type == kWireTypeVarint) {
        std::uint64_t ignored = 0;
        return read_varint(data, length, cursor, ignored);
    }
    if (wire_type == kWireTypeLengthDelimited) {
        std::string ignored;
        return read_length_delimited(data, length, cursor, ignored);
    }
    return false;
}

bool read_next_key(const std::uint8_t* data,
                   std::size_t length,
                   std::size_t& cursor,
                   std::uint32_t& field_out,
                   std::uint32_t& wire_type_out)
{
    std::uint64_t key = 0;
    if (!read_varint(data, length, cursor, key)) {
        return false;
    }
    field_out = static_cast<std::uint32_t>(key >> 3U);
    wire_type_out = static_cast<std::uint32_t>(key & 0x07U);
    return field_out != 0;
}

std::optional<double> parse_decimal(const std::string& value)
{
    if (value.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (!end || *end != '\0') {
        return std::nullopt;
    }
    return parsed;
}

std::string trade_status_text(std::uint64_t status)
{
    switch (status) {
    case 0:
        return "NORMAL";
    case 1:
        return "HALTED";
    case 10:
        return "SUSPEND_TRADE";
    default:
        return std::to_string(status);
    }
}

std::string trade_session_text(std::uint64_t session)
{
    switch (session) {
    case 0:
        return "NORMAL_TRADE";
    case 1:
        return "PRE_TRADE";
    case 2:
        return "POST_TRADE";
    case 3:
        return "OVERNIGHT_TRADE";
    default:
        return std::to_string(session);
    }
}

std::optional<quotes::QuoteSnapshot> decode_security_quote(const std::uint8_t* data, std::size_t length)
{
    std::size_t cursor = 0;
    quotes::QuoteSnapshot snapshot;
    std::string symbol_text;

    while (cursor < length) {
        std::uint32_t field = 0;
        std::uint32_t wire_type = 0;
        if (!read_next_key(data, length, cursor, field, wire_type)) {
            return std::nullopt;
        }

        if (wire_type == kWireTypeLengthDelimited) {
            std::string value;
            if (!read_length_delimited(data, length, cursor, value)) {
                return std::nullopt;
            }
            switch (field) {
            case 1:
                symbol_text = std::move(value);
                break;
            case 2:
                snapshot.last_price = parse_decimal(value);
                break;
            case 3:
                snapshot.previous_close = parse_decimal(value);
                break;
            case 4:
                snapshot.open = parse_decimal(value);
                break;
            case 5:
                snapshot.high = parse_decimal(value);
                break;
            case 6:
                snapshot.low = parse_decimal(value);
                break;
            case 9:
                snapshot.turnover = parse_decimal(value);
                break;
            default:
                break;
            }
        } else if (wire_type == kWireTypeVarint) {
            std::uint64_t value = 0;
            if (!read_varint(data, length, cursor, value)) {
                return std::nullopt;
            }
            switch (field) {
            case 7:
                snapshot.timestamp_ms = static_cast<std::int64_t>(value);
                break;
            case 8:
                snapshot.volume = static_cast<double>(value);
                break;
            case 10:
                snapshot.trade_status = trade_status_text(value);
                break;
            default:
                break;
            }
        } else if (!skip_field(data, length, cursor, wire_type)) {
            return std::nullopt;
        }
    }

    auto symbol = quotes::SecuritySymbol::parse(symbol_text);
    if (!symbol) {
        return std::nullopt;
    }
    snapshot.symbol = *symbol;
    return snapshot;
}

} // namespace

std::vector<std::uint8_t> encode_auth_request(const std::string& token)
{
    std::vector<std::uint8_t> out;
    write_string(out, 1, token);
    return out;
}

std::vector<std::uint8_t> encode_multi_security_request(
    const std::vector<quotes::SecuritySymbol>& symbols)
{
    std::vector<std::uint8_t> out;
    for (const auto& symbol : symbols) {
        write_string(out, 1, symbol.value());
    }
    return out;
}

std::vector<std::uint8_t> encode_subscribe_quote_request(
    const std::vector<quotes::SecuritySymbol>& symbols,
    bool is_first_push)
{
    std::vector<std::uint8_t> out;
    for (const auto& symbol : symbols) {
        write_string(out, 1, symbol.value());
    }
    write_enum(out, 2, kSubTypeQuote);
    write_bool(out, 3, is_first_push);
    return out;
}

std::vector<std::uint8_t> encode_unsubscribe_quote_request(
    const std::vector<quotes::SecuritySymbol>& symbols,
    bool unsubscribe_all)
{
    std::vector<std::uint8_t> out;
    for (const auto& symbol : symbols) {
        write_string(out, 1, symbol.value());
    }
    write_enum(out, 2, kSubTypeQuote);
    write_bool(out, 3, unsubscribe_all);
    return out;
}

std::optional<std::vector<quotes::QuoteSnapshot>> decode_security_quote_response(
    const std::uint8_t* data,
    std::size_t length)
{
    std::size_t cursor = 0;
    std::vector<quotes::QuoteSnapshot> snapshots;

    while (cursor < length) {
        std::uint32_t field = 0;
        std::uint32_t wire_type = 0;
        if (!read_next_key(data, length, cursor, field, wire_type)) {
            return std::nullopt;
        }

        if (field == 1 && wire_type == kWireTypeLengthDelimited) {
            std::vector<std::uint8_t> quote_payload;
            if (!read_length_delimited_bytes(data, length, cursor, quote_payload)) {
                return std::nullopt;
            }
            auto snapshot = decode_security_quote(quote_payload.data(), quote_payload.size());
            if (!snapshot) {
                return std::nullopt;
            }
            snapshots.push_back(std::move(*snapshot));
        } else if (!skip_field(data, length, cursor, wire_type)) {
            return std::nullopt;
        }
    }

    return snapshots;
}

std::optional<quotes::QuoteDelta> decode_push_quote(const std::uint8_t* data, std::size_t length)
{
    std::size_t cursor = 0;
    quotes::QuoteDelta delta;
    std::string symbol_text;

    while (cursor < length) {
        std::uint32_t field = 0;
        std::uint32_t wire_type = 0;
        if (!read_next_key(data, length, cursor, field, wire_type)) {
            return std::nullopt;
        }

        if (wire_type == kWireTypeLengthDelimited) {
            std::string value;
            if (!read_length_delimited(data, length, cursor, value)) {
                return std::nullopt;
            }
            switch (field) {
            case 1:
                symbol_text = std::move(value);
                break;
            case 3:
                delta.last_price = parse_decimal(value);
                break;
            case 4:
                delta.open = parse_decimal(value);
                break;
            case 5:
                delta.high = parse_decimal(value);
                break;
            case 6:
                delta.low = parse_decimal(value);
                break;
            case 9:
                delta.turnover = parse_decimal(value);
                break;
            default:
                break;
            }
        } else if (wire_type == kWireTypeVarint) {
            std::uint64_t value = 0;
            if (!read_varint(data, length, cursor, value)) {
                return std::nullopt;
            }
            switch (field) {
            case 2:
                delta.sequence = value;
                break;
            case 7:
                delta.timestamp_ms = static_cast<std::int64_t>(value);
                break;
            case 8:
                delta.volume = static_cast<double>(value);
                break;
            case 10:
                delta.trade_status = trade_status_text(value);
                break;
            case 11:
                delta.session = trade_session_text(value);
                break;
            default:
                break;
            }
        } else if (!skip_field(data, length, cursor, wire_type)) {
            return std::nullopt;
        }
    }

    auto symbol = quotes::SecuritySymbol::parse(symbol_text);
    if (!symbol) {
        return std::nullopt;
    }
    delta.symbol = *symbol;
    return delta;
}

} // namespace tab5::longbridge
