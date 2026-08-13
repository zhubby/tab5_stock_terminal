#include "longbridge/api_auth.hpp"

#include "auth/sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace tab5::longbridge {
namespace {

constexpr const char* kSignedHeaders = "authorization;x-api-key;x-timestamp";

std::string hex_encode(const std::uint8_t* data, std::size_t length)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(length * 2U);
    for (std::size_t i = 0; i < length; ++i) {
        out[i * 2U] = kHex[(data[i] >> 4U) & 0x0fU];
        out[i * 2U + 1U] = kHex[data[i] & 0x0fU];
    }
    return out;
}

std::array<std::uint8_t, 20> sha1(const std::uint8_t* data, std::size_t length)
{
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xefcdab89U;
    std::uint32_t h2 = 0x98badcfeU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xc3d2e1f0U;

    std::vector<std::uint8_t> padded(data, data + length);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(length) * 8U;
    padded.push_back(0x80U);
    while ((padded.size() % 64U) != 56U) {
        padded.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
    }

    for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
        std::array<std::uint32_t, 80> w {};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t pos = offset + i * 4U;
            w[i] = (static_cast<std::uint32_t>(padded[pos]) << 24U)
                | (static_cast<std::uint32_t>(padded[pos + 1]) << 16U)
                | (static_cast<std::uint32_t>(padded[pos + 2]) << 8U)
                | static_cast<std::uint32_t>(padded[pos + 3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            const std::uint32_t value = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (value << 1U) | (value >> 31U);
        }

        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;

        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }

            const std::uint32_t temp = ((a << 5U) | (a >> 27U)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30U) | (b >> 2U);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest {};
    const std::array<std::uint32_t, 5> words { h0, h1, h2, h3, h4 };
    for (std::size_t i = 0; i < words.size(); ++i) {
        digest[i * 4U] = static_cast<std::uint8_t>((words[i] >> 24U) & 0xffU);
        digest[i * 4U + 1U] = static_cast<std::uint8_t>((words[i] >> 16U) & 0xffU);
        digest[i * 4U + 2U] = static_cast<std::uint8_t>((words[i] >> 8U) & 0xffU);
        digest[i * 4U + 3U] = static_cast<std::uint8_t>(words[i] & 0xffU);
    }
    return digest;
}

std::array<std::uint8_t, 32> hmac_sha256(const std::string& key, const std::string& message)
{
    std::array<std::uint8_t, 64> normalized_key {};
    if (key.size() > normalized_key.size()) {
        const auto digest = auth::sha256(key);
        std::copy(digest.begin(), digest.end(), normalized_key.begin());
    } else {
        std::memcpy(normalized_key.data(), key.data(), key.size());
    }

    std::array<std::uint8_t, 64> inner_pad {};
    std::array<std::uint8_t, 64> outer_pad {};
    for (std::size_t i = 0; i < normalized_key.size(); ++i) {
        inner_pad[i] = normalized_key[i] ^ 0x36U;
        outer_pad[i] = normalized_key[i] ^ 0x5cU;
    }

    std::vector<std::uint8_t> inner;
    inner.reserve(inner_pad.size() + message.size());
    inner.insert(inner.end(), inner_pad.begin(), inner_pad.end());
    inner.insert(inner.end(), message.begin(), message.end());
    const auto inner_digest = auth::sha256(inner.data(), inner.size());

    std::vector<std::uint8_t> outer;
    outer.reserve(outer_pad.size() + inner_digest.size());
    outer.insert(outer.end(), outer_pad.begin(), outer_pad.end());
    outer.insert(outer.end(), inner_digest.begin(), inner_digest.end());
    return auth::sha256(outer.data(), outer.size());
}

bool starts_with(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string dc_region_from_credential(const std::string& credential)
{
    return starts_with(strip_bearer_prefix(credential), "us_") ? "us" : "ap";
}

std::string api_signature(const ApiKeyCredentials& credentials,
                          const std::string& method,
                          const std::string& path,
                          const std::string& query,
                          const std::string& body,
                          std::int64_t timestamp_ms)
{
    const std::string access_token = strip_bearer_prefix(credentials.access_token);
    const std::string app_key = strip_bearer_prefix(credentials.app_key);
    const std::string timestamp = std::to_string(timestamp_ms);
    std::string plain = method;
    std::transform(plain.begin(), plain.end(), plain.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    plain += "|";
    plain += path;
    plain += "|";
    plain += query;
    plain += "|authorization:";
    plain += access_token;
    plain += "\nx-api-key:";
    plain += app_key;
    plain += "\nx-timestamp:";
    plain += timestamp;
    plain += "\n|";
    plain += kSignedHeaders;
    plain += "|";
    if (!body.empty()) {
        const auto body_hash = sha1(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
        plain += hex_encode(body_hash.data(), body_hash.size());
    }

    const auto plain_hash = sha1(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size());
    std::string text_to_sign = "HMAC-SHA256|";
    text_to_sign += hex_encode(plain_hash.data(), plain_hash.size());
    const auto signature = hmac_sha256(credentials.app_secret, text_to_sign);
    return hex_encode(signature.data(), signature.size());
}

} // namespace

std::string strip_bearer_prefix(const std::string& credential)
{
    constexpr const char* prefix = "Bearer ";
    if (starts_with(credential, prefix)) {
        return credential.substr(std::strlen(prefix));
    }
    return credential;
}

std::string dc_region_from_credentials(const std::vector<std::string>& credentials)
{
    for (const auto& credential : credentials) {
        if (dc_region_from_credential(credential) == "us") {
            return "us";
        }
    }
    return "ap";
}

std::vector<HttpHeader> build_longbridge_auth_headers(const ApiKeyCredentials& credentials,
                                                      const std::string& method,
                                                      const std::string& path,
                                                      const std::string& query,
                                                      const std::string& body,
                                                      std::int64_t timestamp_ms)
{
    std::vector<HttpHeader> headers;
    const std::string app_key = strip_bearer_prefix(credentials.app_key);
    const std::string access_token = strip_bearer_prefix(credentials.access_token);
    const std::string timestamp = std::to_string(timestamp_ms);
    headers.push_back({ "authorization", access_token });
    headers.push_back({ "x-api-key", app_key });
    headers.push_back({ "x-dc-region",
                        dc_region_from_credentials({
                            credentials.app_key,
                            credentials.app_secret,
                            credentials.access_token,
                        }) });
    headers.push_back({ "x-timestamp", timestamp });
    headers.push_back({
        "x-api-signature",
        std::string("HMAC-SHA256 SignedHeaders=") + kSignedHeaders + ", Signature="
            + api_signature(credentials, method, path, query, body, timestamp_ms),
    });
    return headers;
}

} // namespace tab5::longbridge
