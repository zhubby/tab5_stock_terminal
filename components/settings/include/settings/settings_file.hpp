#pragma once

#include "settings/app_settings.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tab5::settings {

enum class SettingsFileStatus {
    Ok,
    Empty,
    MissingRequired,
    InvalidWatchlist,
    InvalidEndpoint,
};

struct SettingsFileResult {
    SettingsFileStatus status { SettingsFileStatus::Empty };
    AppSettings settings;
    std::vector<std::string> warnings;
    bool touched_oauth_tokens { false };
    bool touched_api_key_credentials { false };
    bool reset_tokens { false };

    bool ok() const { return status == SettingsFileStatus::Ok; }
};

SettingsFileResult parse_settings_file(const std::string& content);
const char* settings_file_status_text(SettingsFileStatus status);

} // namespace tab5::settings
