#include "auth/oauth_callback_server.hpp"

#include <chrono>
#include <utility>

#if !defined(TAB5_HOST_TEST)
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#endif

namespace tab5::auth {
namespace {

#if !defined(TAB5_HOST_TEST)
constexpr const char* kTag = "oauth-callback";

esp_err_t callback_handler(httpd_req_t* request)
{
    auto* server = static_cast<OAuthCallbackServer*>(request->user_ctx);
    char query[768] {};
    const esp_err_t query_err = httpd_req_get_url_query_str(request, query, sizeof(query));

    const bool accepted = server && query_err == ESP_OK && server->handle_query(query);
    const char* response = accepted
        ? "<html><body><h1>Authorized</h1><p>You can return to the Tab5.</p></body></html>"
        : "<html><body><h1>Authorization rejected</h1><p>State or code was invalid.</p></body></html>";
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_status(request, accepted ? "200 OK" : "400 Bad Request");
    httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
#endif

std::int64_t now_us()
{
#if defined(TAB5_HOST_TEST)
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
#else
    return esp_timer_get_time();
#endif
}

} // namespace

OAuthCallbackServer::~OAuthCallbackServer()
{
    stop();
}

bool OAuthCallbackServer::start(std::uint16_t port,
                                std::string expected_state,
                                Handler handler,
                                std::uint32_t ttl_seconds)
{
    stop();
    expected_state_ = std::move(expected_state);
    handler_ = std::move(handler);
    consumed_ = false;
    expires_at_us_ = now_us() + static_cast<std::int64_t>(ttl_seconds) * 1'000'000;

#if defined(TAB5_HOST_TEST)
    running_ = true;
    return true;
#else
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t handle = nullptr;
    const esp_err_t err = httpd_start(&handle, &config);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "failed to start callback server: %s", esp_err_to_name(err));
        return false;
    }

    httpd_uri_t uri {};
    uri.uri = "/oauth/callback*";
    uri.method = HTTP_GET;
    uri.handler = callback_handler;
    uri.user_ctx = this;

    const esp_err_t register_err = httpd_register_uri_handler(handle, &uri);
    if (register_err != ESP_OK) {
        ESP_LOGW(kTag, "failed to register callback handler: %s", esp_err_to_name(register_err));
        httpd_stop(handle);
        return false;
    }

    server_ = handle;
    running_ = true;
    return true;
#endif
}

void OAuthCallbackServer::stop()
{
#if !defined(TAB5_HOST_TEST)
    if (server_) {
        httpd_stop(static_cast<httpd_handle_t>(server_));
    }
#endif
    server_ = nullptr;
    running_ = false;
    consumed_ = false;
    expires_at_us_ = 0;
}

bool OAuthCallbackServer::handle_query(const std::string& query)
{
    if (!running_ || consumed_ || now_us() > expires_at_us_) {
        return false;
    }

    auto params = parse_oauth_callback_query(query);
    if (!params.has_code() || params.state != expected_state_) {
        return false;
    }

    consumed_ = true;
    expected_state_.clear();
    auto handler = std::move(handler_);
    if (handler) {
        handler(params);
    }
    return true;
}

} // namespace tab5::auth
