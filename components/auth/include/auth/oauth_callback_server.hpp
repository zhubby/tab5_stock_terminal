#pragma once

#include "auth/oauth.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace tab5::auth {

class OAuthCallbackServer {
public:
    using Handler = std::function<void(const OAuthCallbackParams&)>;

    OAuthCallbackServer() = default;
    ~OAuthCallbackServer();

    bool start(std::uint16_t port,
               std::string expected_state,
               Handler handler,
               std::uint32_t ttl_seconds = 300);
    void stop();
    bool running() const { return running_; }
    bool handle_query(const std::string& query);

private:
    std::string expected_state_;
    Handler handler_;
    bool running_ { false };
    bool consumed_ { false };
    std::int64_t expires_at_us_ { 0 };
    void* server_ { nullptr };
};

} // namespace tab5::auth
