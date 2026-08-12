#pragma once

#include "auth/oauth.hpp"
#include "longbridge/endpoint.hpp"
#include "quotes/symbol.hpp"

#include <cstdint>
#include <string>

namespace tab5::settings {

struct WifiCredentials {
    std::string ssid;
    std::string password;

    bool complete() const { return !ssid.empty(); }
};

struct AppSettings {
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schema_version { kSchemaVersion };
    WifiCredentials wifi;
    longbridge::EndpointRegion endpoint_region { longbridge::EndpointRegion::Global };
    std::string longbridge_client_id;
    std::string oauth_redirect_uri { "http://tab5-stock.local/oauth/callback" };
    auth::OAuthTokens oauth_tokens;
    quotes::Watchlist watchlist;

    bool onboarding_complete() const
    {
        return wifi.complete() && !longbridge_client_id.empty() && !oauth_tokens.empty() && !watchlist.empty();
    }
};

} // namespace tab5::settings
