#pragma once

#include "quotes/quote_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tab5::longbridge {

std::vector<std::uint8_t> encode_auth_request(const std::string& token);
std::vector<std::uint8_t> encode_multi_security_request(
    const std::vector<quotes::SecuritySymbol>& symbols);
std::vector<std::uint8_t> encode_subscribe_quote_request(
    const std::vector<quotes::SecuritySymbol>& symbols,
    bool is_first_push);
std::vector<std::uint8_t> encode_unsubscribe_quote_request(
    const std::vector<quotes::SecuritySymbol>& symbols,
    bool unsubscribe_all = false);
std::optional<std::vector<quotes::QuoteSnapshot>> decode_security_quote_response(
    const std::uint8_t* data,
    std::size_t length);
std::optional<quotes::QuoteDelta> decode_push_quote(const std::uint8_t* data, std::size_t length);

} // namespace tab5::longbridge
