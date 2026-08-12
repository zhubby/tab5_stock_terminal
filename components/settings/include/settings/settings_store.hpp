#pragma once

#include "settings/app_settings.hpp"

#include <string>

#if __has_include("esp_err.h") && !defined(TAB5_HOST_TEST)
#include "esp_err.h"
#else
using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0;
static constexpr esp_err_t ESP_FAIL = -1;
#endif

namespace tab5::settings {

class SettingsStore {
public:
    explicit SettingsStore(std::string nvs_namespace = "tab5_stock");

    esp_err_t initialize();
    esp_err_t load(AppSettings& settings_out);
    esp_err_t save(const AppSettings& settings);
    esp_err_t reset();

private:
    std::string nvs_namespace_;
};

} // namespace tab5::settings
