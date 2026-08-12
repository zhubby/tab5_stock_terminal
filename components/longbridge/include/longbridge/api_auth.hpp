#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tab5::longbridge {

struct ApiKeyCredentials {
    std::string app_key;
    std::string app_secret;
    std::string access_token;

    bool complete() const
    {
        return !app_key.empty() && !app_secret.empty() && !access_token.empty();
    }
};

enum class HttpAuthMode {
    OAuth,
    ApiKey,
};

struct HttpAuthConfig {
    HttpAuthMode mode { HttpAuthMode::OAuth };
    std::string oauth_access_token;
    ApiKeyCredentials api_key;
};

struct HttpHeader {
    std::string name;
    std::string value;
};

std::string strip_bearer_prefix(const std::string& credential);
std::string dc_region_from_credentials(const std::vector<std::string>& credentials);
std::vector<HttpHeader> build_longbridge_auth_headers(const HttpAuthConfig& auth,
                                                      const std::string& method,
                                                      const std::string& path,
                                                      const std::string& query,
                                                      const std::string& body,
                                                      std::int64_t timestamp_ms);

} // namespace tab5::longbridge
