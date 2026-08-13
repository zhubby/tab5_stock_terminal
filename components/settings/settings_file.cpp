#include "settings/settings_file.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>

namespace tab5::settings {
namespace {

std::string trim(std::string_view input)
{
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front()))) {
        input.remove_prefix(1);
    }
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back()))) {
        input.remove_suffix(1);
    }
    return std::string(input);
}

std::string lower(std::string input)
{
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return input;
}

std::string normalized_key(std::string input)
{
    input = lower(trim(input));
    std::replace(input.begin(), input.end(), '-', '_');
    return input;
}

bool starts_with(const std::string& value, char prefix)
{
    return !value.empty() && value.front() == prefix;
}

std::optional<longbridge::EndpointRegion> parse_endpoint_region(const std::string& value)
{
    const std::string normalized = lower(trim(value));
    if (normalized == "global" || normalized == "com" || normalized == ".com") {
        return longbridge::EndpointRegion::Global;
    }
    if (normalized == "cn" || normalized == ".cn" || normalized == "china" || normalized == "mainland") {
        return longbridge::EndpointRegion::MainlandChina;
    }
    return std::nullopt;
}

void add_watchlist_symbol(quotes::Watchlist& watchlist,
                          const std::string& value,
                          std::vector<std::string>& warnings)
{
    const std::string symbol_text = trim(value);
    if (symbol_text.empty()) {
        return;
    }
    const auto symbol = quotes::SecuritySymbol::parse(symbol_text);
    if (!symbol) {
        warnings.push_back("ignored invalid watchlist symbol: " + symbol_text);
        return;
    }
    const auto result = watchlist.add(*symbol);
    if (result == quotes::WatchlistAddResult::Full) {
        warnings.push_back("watchlist cap reached; ignored: " + symbol->value());
    }
}

void add_watchlist_values(quotes::Watchlist& watchlist,
                          const std::string& value,
                          std::vector<std::string>& warnings)
{
    std::string token;
    std::istringstream stream(value);
    while (std::getline(stream, token, ',')) {
        add_watchlist_symbol(watchlist, token, warnings);
    }
}

void apply_key_value(SettingsFileResult& result,
                     const std::string& key,
                     const std::string& value,
                     bool legacy_oauth_mode,
                     bool& append_watchlist)
{
    AppSettings& settings = result.settings;
    const std::string normalized = normalized_key(key);
    const std::string clean_value = trim(value);
    if (normalized == "wifi_ssid" || normalized == "ssid") {
        settings.wifi.ssid = clean_value;
    } else if (normalized == "wifi_password" || normalized == "wifi_pass" || normalized == "password") {
        settings.wifi.password = clean_value;
    } else if (normalized == "endpoint" || normalized == "endpoint_region" || normalized == "longbridge_endpoint") {
        settings.endpoint_region = longbridge::endpoint_region_from_string(clean_value);
    } else if (normalized == "auth_mode" || normalized == "auth" || normalized == "longbridge_auth") {
        result.warnings.push_back("ignored legacy auth mode; only API token is supported: " + clean_value);
    } else if (normalized == "client_id" || normalized == "longbridge_client_id"
               || normalized == "redirect_uri" || normalized == "oauth_redirect_uri"
               || normalized == "callback_uri" || normalized == "oauth_access_token"
               || normalized == "refresh_token" || normalized == "oauth_refresh_token"
               || normalized == "token_expires_at" || normalized == "expires_at"
               || normalized == "oauth_expires_at" || normalized == "reset_tokens") {
        result.warnings.push_back("ignored OAuth key: " + key);
    } else if (normalized == "access_token" && legacy_oauth_mode) {
        result.warnings.push_back("ignored legacy OAuth key: " + key);
    } else if (normalized == "access_token") {
        result.touched_api_key_credentials = true;
        settings.api_key.access_token = clean_value;
    } else if (normalized == "app_key" || normalized == "longbridge_app_key" || normalized == "api_app_key") {
        result.touched_api_key_credentials = true;
        settings.api_key.app_key = clean_value;
    } else if (normalized == "app_secret" || normalized == "longbridge_app_secret" || normalized == "api_app_secret") {
        result.touched_api_key_credentials = true;
        settings.api_key.app_secret = clean_value;
    } else if (normalized == "api_access_token" || normalized == "api_token"
               || normalized == "longbridge_access_token") {
        result.touched_api_key_credentials = true;
        settings.api_key.access_token = clean_value;
    } else if (normalized == "watchlist") {
        if (!append_watchlist) {
            settings.watchlist.clear();
            append_watchlist = true;
        }
        add_watchlist_values(settings.watchlist, clean_value, result.warnings);
    } else if (normalized == "symbol") {
        append_watchlist = true;
        add_watchlist_symbol(settings.watchlist, clean_value, result.warnings);
    } else {
        result.warnings.push_back("ignored unknown key: " + key);
    }
}

bool has_legacy_oauth_mode(const std::string& content)
{
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string clean = trim(line);
        if (clean.empty() || starts_with(clean, '#') || starts_with(clean, ';')) {
            continue;
        }
        if (starts_with(clean, '[') && clean.back() == ']') {
            continue;
        }
        const auto equals = clean.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string normalized = normalized_key(clean.substr(0, equals));
        if (normalized == "auth_mode" || normalized == "auth" || normalized == "longbridge_auth") {
            const std::string value = lower(trim(clean.substr(equals + 1)));
            return value == "oauth" || value == "oauth2";
        }
    }
    return false;
}

bool required_present(const AppSettings& settings)
{
    if (!settings.wifi.complete() || settings.watchlist.empty()) {
        return false;
    }
    return settings.api_key.complete();
}

} // namespace

SettingsFileResult parse_settings_file(const std::string& content)
{
    SettingsFileResult result;
    result.settings = AppSettings {};
    bool saw_data = false;
    bool invalid_endpoint = false;
    bool append_watchlist = false;
    const bool legacy_oauth_mode = has_legacy_oauth_mode(content);

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string clean = trim(line);
        if (clean.empty() || starts_with(clean, '#') || starts_with(clean, ';')) {
            continue;
        }
        if (starts_with(clean, '[') && clean.back() == ']') {
            continue;
        }

        const auto equals = clean.find('=');
        if (equals == std::string::npos) {
            result.warnings.push_back("ignored line without '=': " + clean);
            continue;
        }

        saw_data = true;
        const std::string key = clean.substr(0, equals);
        const std::string normalized = normalized_key(key);
        const std::string value = trim(clean.substr(equals + 1));
        if (normalized == "endpoint" || normalized == "endpoint_region" || normalized == "longbridge_endpoint") {
            const auto endpoint = parse_endpoint_region(value);
            if (!endpoint) {
                result.warnings.push_back("invalid endpoint: " + value);
                invalid_endpoint = true;
                continue;
            }
            result.settings.endpoint_region = *endpoint;
            continue;
        }
        apply_key_value(result, key, value, legacy_oauth_mode, append_watchlist);
    }

    if (!saw_data) {
        result.status = SettingsFileStatus::Empty;
        return result;
    }
    if (invalid_endpoint) {
        result.status = SettingsFileStatus::InvalidEndpoint;
        return result;
    }
    if (result.settings.watchlist.empty()) {
        result.status = SettingsFileStatus::InvalidWatchlist;
        return result;
    }
    if (!required_present(result.settings)) {
        result.status = SettingsFileStatus::MissingRequired;
        return result;
    }

    result.status = SettingsFileStatus::Ok;
    return result;
}

const char* settings_file_status_text(SettingsFileStatus status)
{
    switch (status) {
    case SettingsFileStatus::Ok:
        return "ok";
    case SettingsFileStatus::Empty:
        return "empty";
    case SettingsFileStatus::MissingRequired:
        return "missing required keys";
    case SettingsFileStatus::InvalidWatchlist:
        return "invalid watchlist";
    case SettingsFileStatus::InvalidEndpoint:
        return "invalid endpoint";
    }
    return "unknown";
}

} // namespace tab5::settings
