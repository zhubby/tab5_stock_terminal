#pragma once

#include "auth/oauth.hpp"
#include "longbridge/endpoint.hpp"
#include "quotes/symbol.hpp"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <string>

namespace tab5::settings {

enum class AuthMode {
    OAuth,
    ApiKey,
};

struct WifiCredentials {
    std::string ssid;
    std::string password;

    bool complete() const { return !ssid.empty(); }
};

struct LongbridgeApiKeyCredentials {
    std::string app_key;
    std::string app_secret;
    std::string access_token;

    bool complete() const
    {
        return !app_key.empty() && !app_secret.empty() && !access_token.empty();
    }
};

struct AppSettings {
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schema_version { kSchemaVersion };
    WifiCredentials wifi;
    longbridge::EndpointRegion endpoint_region { longbridge::EndpointRegion::Global };
    AuthMode auth_mode { AuthMode::OAuth };
    std::string longbridge_client_id;
    std::string oauth_redirect_uri { "http://tab5-stock.local/oauth/callback" };
    auth::OAuthTokens oauth_tokens;
    LongbridgeApiKeyCredentials api_key;
    quotes::Watchlist watchlist;

    bool quote_auth_configured() const
    {
        return auth_mode == AuthMode::ApiKey ? api_key.complete() : !oauth_tokens.empty();
    }

    bool onboarding_complete() const
    {
        return wifi.complete() && quote_auth_configured() && !watchlist.empty();
    }
};

inline const char* to_string(AuthMode mode)
{
    switch (mode) {
    case AuthMode::ApiKey:
        return "api_key";
    case AuthMode::OAuth:
    default:
        return "oauth";
    }
}

inline AuthMode auth_mode_from_string(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "api_key" || value == "apikey" || value == "api-token" || value == "api_token"
        || value == "legacy") {
        return AuthMode::ApiKey;
    }
    return AuthMode::OAuth;
}

} // namespace tab5::settings
