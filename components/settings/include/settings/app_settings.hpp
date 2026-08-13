#pragma once

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
    LongbridgeApiKeyCredentials api_key;
    quotes::Watchlist watchlist;

    bool quote_auth_configured() const
    {
        return api_key.complete();
    }

    bool onboarding_complete() const
    {
        return wifi.complete() && quote_auth_configured() && !watchlist.empty();
    }
};

} // namespace tab5::settings
