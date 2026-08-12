#include "auth/oauth.hpp"
#include "auth/sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <random>
#include <sstream>

#if __has_include("esp_random.h") && !defined(TAB5_HOST_TEST)
#include "esp_random.h"
#endif

namespace tab5::auth {
namespace {

constexpr char kVerifierAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

std::uint32_t entropy_word()
{
#if __has_include("esp_random.h") && !defined(TAB5_HOST_TEST)
    return esp_random();
#else
    static std::random_device device;
    static std::mt19937 generator(device());
    return generator();
#endif
}

void append_query_param(std::ostringstream& out,
                        bool& first,
                        const std::string& key,
                        const std::string& value)
{
    out << (first ? '?' : '&');
    first = false;
    out << key << '=' << url_encode(value);
}

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

} // namespace

bool OAuthTokens::expired(std::int64_t now_epoch_s) const
{
    return empty() || expires_at_epoch_s <= now_epoch_s;
}

bool OAuthTokens::refresh_due(std::int64_t now_epoch_s) const
{
    if (empty()) {
        return false;
    }
    constexpr std::int64_t kRefreshSkewSeconds = 120;
    return expires_at_epoch_s <= now_epoch_s + kRefreshSkewSeconds;
}

std::string base64url_encode(const std::uint8_t* data, std::size_t length)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string out;
    out.reserve(((length + 2) / 3) * 4);

    for (std::size_t i = 0; i < length; i += 3) {
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = (i + 1 < length) ? data[i + 1] : 0;
        const std::uint32_t b2 = (i + 2 < length) ? data[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16U) | (b1 << 8U) | b2;

        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        if (i + 1 < length) {
            out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
        }
        if (i + 2 < length) {
            out.push_back(kAlphabet[triple & 0x3FU]);
        }
    }

    return out;
}

std::string url_encode(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());

    for (unsigned char ch : value) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
            || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'
            || ch == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[(ch >> 4U) & 0x0FU]);
            out.push_back(kHex[ch & 0x0FU]);
        }
    }

    return out;
}

std::string url_decode(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+' ) {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hex_value(value[i + 1]);
            const int lo = hex_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(value[i]);
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string pkce_challenge_for_verifier(const std::string& verifier)
{
    const auto digest = sha256(verifier);
    return base64url_encode(digest.data(), digest.size());
}

PkcePair make_pkce_pair(std::size_t verifier_length)
{
    verifier_length = std::clamp<std::size_t>(verifier_length, 43, 128);
    std::string verifier;
    verifier.reserve(verifier_length);

    for (std::size_t i = 0; i < verifier_length; ++i) {
        const auto index = entropy_word() % (sizeof(kVerifierAlphabet) - 1);
        verifier.push_back(kVerifierAlphabet[index]);
    }

    return { verifier, pkce_challenge_for_verifier(verifier) };
}

std::string build_authorization_url(const OAuthConfig& config,
                                    const std::string& state,
                                    const std::string& code_challenge)
{
    std::ostringstream out;
    out << config.authorize_endpoint;
    bool first = config.authorize_endpoint.find('?') == std::string::npos;

    append_query_param(out, first, "response_type", "code");
    append_query_param(out, first, "client_id", config.client_id);
    append_query_param(out, first, "redirect_uri", config.redirect_uri);
    append_query_param(out, first, "scope", config.scope);
    append_query_param(out, first, "state", state);
    append_query_param(out, first, "code_challenge", code_challenge);
    append_query_param(out, first, "code_challenge_method", "S256");

    return out.str();
}

OAuthCallbackParams parse_oauth_callback_query(const std::string& query)
{
    OAuthCallbackParams params;
    const std::size_t start = (!query.empty() && query.front() == '?') ? 1 : 0;
    std::size_t cursor = start;
    while (cursor <= query.size()) {
        const std::size_t amp = query.find('&', cursor);
        const std::size_t end = amp == std::string::npos ? query.size() : amp;
        const auto part = query.substr(cursor, end - cursor);
        const auto eq = part.find('=');
        if (eq != std::string::npos) {
            const auto key = url_decode(part.substr(0, eq));
            const auto value = url_decode(part.substr(eq + 1));
            if (key == "code") {
                params.code = value;
            } else if (key == "state") {
                params.state = value;
            } else if (key == "error") {
                params.error = value;
            } else if (key == "error_description") {
                params.error_description = value;
            }
        }
        if (amp == std::string::npos) {
            break;
        }
        cursor = amp + 1;
    }
    return params;
}

} // namespace tab5::auth
