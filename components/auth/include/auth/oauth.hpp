#pragma once

#include <cstdint>
#include <string>

namespace tab5::auth {

struct PkcePair {
    std::string verifier;
    std::string challenge;
};

struct OAuthConfig {
    std::string authorize_endpoint;
    std::string token_endpoint;
    std::string client_id;
    std::string redirect_uri;
    std::string scope;
};

struct OAuthTokens {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at_epoch_s { 0 };

    bool empty() const { return access_token.empty() || refresh_token.empty(); }
    bool expired(std::int64_t now_epoch_s) const;
    bool refresh_due(std::int64_t now_epoch_s) const;
};

struct OAuthCallbackParams {
    std::string code;
    std::string state;
    std::string error;
    std::string error_description;

    bool has_code() const { return !code.empty() && error.empty(); }
};

std::string base64url_encode(const std::uint8_t* data, std::size_t length);
std::string url_encode(const std::string& value);
std::string url_decode(const std::string& value);
std::string pkce_challenge_for_verifier(const std::string& verifier);
PkcePair make_pkce_pair(std::size_t verifier_length = 64);
std::string build_authorization_url(const OAuthConfig& config,
                                    const std::string& state,
                                    const std::string& code_challenge);
OAuthCallbackParams parse_oauth_callback_query(const std::string& query);

} // namespace tab5::auth
