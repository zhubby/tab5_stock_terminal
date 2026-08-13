#include "settings/settings_store.hpp"

#include <utility>

#if !defined(TAB5_HOST_TEST)
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#endif

namespace tab5::settings {
namespace {

constexpr const char* kKeySchema = "schema";
constexpr const char* kKeyWifiSsid = "wifi_ssid";
constexpr const char* kKeyWifiPassword = "wifi_pass";
constexpr const char* kKeyEndpointRegion = "lb_region";
constexpr const char* kKeyApiAppKey = "api_key";
constexpr const char* kKeyApiAppSecret = "api_secret";
constexpr const char* kKeyApiAccessToken = "api_token";
constexpr const char* kKeyWatchlist = "watchlist";
constexpr const char* kLegacyAuthMode = "auth_mode";
constexpr const char* kLegacyClientId = "client_id";
constexpr const char* kLegacyRedirectUri = "redirect";
constexpr const char* kLegacyAccessToken = "access";
constexpr const char* kLegacyRefreshToken = "refresh";
constexpr const char* kLegacyTokenExpiry = "expires";

#if !defined(TAB5_HOST_TEST)
constexpr const char* kTag = "settings";

std::string get_string(nvs_handle_t handle, const char* key)
{
    std::size_t length = 0;
    esp_err_t err = nvs_get_str(handle, key, nullptr, &length);
    if (err != ESP_OK || length == 0) {
        return {};
    }

    std::string value(length, '\0');
    err = nvs_get_str(handle, key, value.data(), &length);
    if (err != ESP_OK) {
        return {};
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

esp_err_t erase_key_if_present(nvs_handle_t handle, const char* key)
{
    const esp_err_t err = nvs_erase_key(handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    return ESP_OK;
}

esp_err_t erase_legacy_oauth_keys(nvs_handle_t handle)
{
    for (const char* key : {
             kLegacyAuthMode,
             kLegacyClientId,
             kLegacyRedirectUri,
             kLegacyAccessToken,
             kLegacyRefreshToken,
             kLegacyTokenExpiry,
         }) {
        const esp_err_t err = erase_key_if_present(handle, key);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
#endif

} // namespace

SettingsStore::SettingsStore(std::string nvs_namespace)
    : nvs_namespace_(std::move(nvs_namespace))
{
}

esp_err_t SettingsStore::initialize()
{
#if defined(TAB5_HOST_TEST)
    return ESP_OK;
#else
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
#endif
}

esp_err_t SettingsStore::load(AppSettings& settings_out)
{
    settings_out = AppSettings {};

#if defined(TAB5_HOST_TEST)
    return ESP_FAIL;
#else
    nvs_handle_t handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    std::uint32_t schema = 0;
    nvs_get_u32(handle, kKeySchema, &schema);
    settings_out.schema_version = schema == 0 ? AppSettings::kSchemaVersion : schema;
    settings_out.wifi.ssid = get_string(handle, kKeyWifiSsid);
    settings_out.wifi.password = get_string(handle, kKeyWifiPassword);
    settings_out.endpoint_region =
        longbridge::endpoint_region_from_string(get_string(handle, kKeyEndpointRegion));
    settings_out.api_key.app_key = get_string(handle, kKeyApiAppKey);
    settings_out.api_key.app_secret = get_string(handle, kKeyApiAppSecret);
    settings_out.api_key.access_token = get_string(handle, kKeyApiAccessToken);
    settings_out.watchlist = quotes::Watchlist::deserialize(get_string(handle, kKeyWatchlist));

    err = erase_legacy_oauth_keys(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    } else {
        ESP_LOGW(kTag, "failed to erase legacy OAuth NVS keys: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
    return ESP_OK;
#endif
}

esp_err_t SettingsStore::save(const AppSettings& settings)
{
#if defined(TAB5_HOST_TEST)
    (void)settings;
    return ESP_FAIL;
#else
    nvs_handle_t handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(handle, kKeySchema, AppSettings::kSchemaVersion);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyWifiSsid, settings.wifi.ssid.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyWifiPassword, settings.wifi.password.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyEndpointRegion, longbridge::to_string(settings.endpoint_region).c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyApiAppKey, settings.api_key.app_key.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyApiAppSecret, settings.api_key.app_secret.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyApiAccessToken, settings.api_key.access_token.c_str());
    }
    if (err == ESP_OK) {
        err = erase_legacy_oauth_keys(handle);
    }
    if (err == ESP_OK) {
        const auto serialized_watchlist = settings.watchlist.serialize();
        err = nvs_set_str(handle, kKeyWatchlist, serialized_watchlist.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
#endif
}

esp_err_t SettingsStore::reset()
{
#if defined(TAB5_HOST_TEST)
    return ESP_OK;
#else
    nvs_handle_t handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
#endif
}

} // namespace tab5::settings
