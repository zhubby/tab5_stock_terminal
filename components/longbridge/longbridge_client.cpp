#include "longbridge/longbridge_client.hpp"
#include "longbridge/quote_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <sstream>

#if !defined(TAB5_HOST_TEST)
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#endif

namespace tab5::longbridge {
namespace {

#if !defined(TAB5_HOST_TEST)
constexpr const char* kTag = "longbridge";
constexpr EventBits_t kWsConnectedBit = BIT0;
constexpr EventBits_t kWsErrorBit = BIT1;
constexpr EventBits_t kWsResponseBit = BIT2;
constexpr std::uint8_t kWireStatusOk = 0;

struct HttpBody {
    std::string body;
};

esp_err_t http_event_handler(esp_http_client_event_t* event)
{
    auto* context = static_cast<HttpBody*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && context && event->data && event->data_len > 0) {
        context->body.append(static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

ClientResult map_status(int status)
{
    if (status >= 200 && status < 300) {
        return {};
    }
    if (status == 401 || status == 403) {
        return {
            status == 403 ? ClientError::PermissionDenied : ClientError::Unauthorized,
            status,
            status == 403 ? "Longbridge permission denied" : "Longbridge rejected the access token",
        };
    }
    if (status == 429) {
        return { ClientError::RateLimited, status, "Longbridge rate limit reached" };
    }
    return { ClientError::Network, status, "Longbridge HTTP request failed" };
}

std::optional<std::string> json_string(cJSON* object, const char* key)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring) {
        return std::string(value->valuestring);
    }
    return std::nullopt;
}

std::int64_t json_int64(cJSON* object, const char* key)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(value)) {
        return static_cast<std::int64_t>(value->valuedouble);
    }
    return 0;
}

std::optional<ClientResult> application_error_from_body(const std::string& body, int http_status)
{
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return std::nullopt;
    }

    ClientResult result;
    result.http_status = http_status;

    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    auto message = json_string(root, "message");
    if (!message) {
        message = json_string(root, "msg");
    }
    if (!message) {
        message = json_string(root, "error");
    }

    bool has_error = false;
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        has_error = true;
        result.message = message.value_or("Longbridge returned an application error");
    } else if (!cJSON_IsNumber(code) && message && cJSON_GetObjectItemCaseSensitive(root, "error")) {
        has_error = true;
        result.message = *message;
    }

    if (has_error) {
        std::string lower_message = result.message;
        std::transform(lower_message.begin(), lower_message.end(), lower_message.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (http_status == 401) {
            result.error = ClientError::Unauthorized;
        } else if (http_status == 403 || lower_message.find("permission") != std::string::npos
                   || lower_message.find("quote") != std::string::npos) {
            result.error = ClientError::PermissionDenied;
        } else if (http_status == 429 || lower_message.find("rate") != std::string::npos) {
            result.error = ClientError::RateLimited;
        } else {
            result.error = ClientError::Protocol;
        }
    }

    cJSON_Delete(root);
    if (!has_error) {
        return std::nullopt;
    }
    return result;
}

ClientResult perform_get(const std::string& url,
                         const std::string& path,
                         const HttpAuthConfig& auth,
                         std::string& body_out,
                         int& status_out)
{
    HttpBody context;
    esp_http_client_config_t config {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.event_handler = http_event_handler;
    config.user_data = &context;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return { ClientError::Network, 0, "failed to initialize HTTP client" };
    }

    const auto auth_headers =
        build_longbridge_auth_headers(auth, "GET", path, "", "", static_cast<std::int64_t>(std::time(nullptr)) * 1000);
    for (const auto& header : auth_headers) {
        esp_http_client_set_header(client, header.name.c_str(), header.value.c_str());
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    const esp_err_t err = esp_http_client_perform(client);
    status_out = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return { ClientError::Network, status_out, esp_err_to_name(err) };
    }

    body_out = std::move(context.body);
    const auto status_result = map_status(status_out);
    if (!status_result.ok()) {
        return status_result;
    }
    if (auto application_error = application_error_from_body(body_out, status_out)) {
        return *application_error;
    }
    return {};
}

ClientResult perform_post_form(const std::string& url,
                               const std::string& form_body,
                               std::string& body_out,
                               int& status_out)
{
    HttpBody context;
    esp_http_client_config_t config {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.user_data = &context;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return { ClientError::Network, 0, "failed to initialize HTTP client" };
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, form_body.c_str(), static_cast<int>(form_body.size()));

    const esp_err_t err = esp_http_client_perform(client);
    status_out = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return { ClientError::Network, status_out, esp_err_to_name(err) };
    }

    body_out = std::move(context.body);
    const auto status_result = map_status(status_out);
    if (!status_result.ok()) {
        return status_result;
    }
    if (auto application_error = application_error_from_body(body_out, status_out)) {
        return *application_error;
    }
    return {};
}

ClientResult result_from_esp_error(esp_err_t err, const char* context)
{
    if (err == ESP_OK) {
        return {};
    }
    return { ClientError::Network, 0, std::string(context) + ": " + esp_err_to_name(err) };
}

ClientResult result_from_wire_status(std::uint8_t status_code,
                                     CommandIntent command,
                                     const std::vector<std::uint8_t>& payload)
{
    if (status_code == kWireStatusOk) {
        return {};
    }

    std::ostringstream message;
    message << "Longbridge command " << static_cast<int>(command)
            << " failed with socket status " << static_cast<int>(status_code);
    if (!payload.empty()) {
        message << " payload_bytes=" << payload.size();
    }

    switch (status_code) {
    case 1:
        return { ClientError::Network, 0, message.str() };
    case 3:
        return { ClientError::Protocol, 0, message.str() };
    case 5:
        return { ClientError::Unauthorized, 0, message.str() };
    case 7:
        return { ClientError::Network, 0, message.str() };
    default:
        return { ClientError::Protocol, 0, message.str() };
    }
}

std::string socket_url_with_handshake_query(std::string url)
{
    url += url.find('?') == std::string::npos ? '?' : '&';
    url += "version=1&codec=1&platform=9";
    return url;
}

std::string host_from_url(const std::string& url)
{
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return {};
    }
    const std::size_t host_start = scheme_end + 3;
    const std::size_t host_end = url.find_first_of(":/?", host_start);
    return url.substr(host_start,
                      host_end == std::string::npos ? std::string::npos : host_end - host_start);
}

bool is_allowed_quote_socket_url(const std::string& url, const EndpointSet& endpoints)
{
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos || url.substr(0, scheme_end) != "wss") {
        return false;
    }
    return host_from_url(url) == host_from_url(endpoints.quote_ws_url);
}

bool parse_oauth_tokens(const std::string& body, auth::OAuthTokens& tokens_out)
{
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* source = cJSON_IsObject(data) ? data : root;

    const auto access = json_string(source, "access_token");
    auto refresh = json_string(source, "refresh_token");
    if (!refresh) {
        refresh = json_string(source, "refreshToken");
    }
    if (!access || !refresh) {
        cJSON_Delete(root);
        return false;
    }

    tokens_out.access_token = *access;
    tokens_out.refresh_token = *refresh;
    const std::int64_t expires_at = json_int64(source, "expires_at");
    const std::int64_t expires_in = json_int64(source, "expires_in");
    if (expires_at > 0) {
        tokens_out.expires_at_epoch_s = expires_at;
    } else if (expires_in > 0) {
        tokens_out.expires_at_epoch_s = static_cast<std::int64_t>(std::time(nullptr)) + expires_in;
    }

    cJSON_Delete(root);
    return true;
}
#endif

std::string token_exchange_form(const auth::OAuthConfig& oauth_config,
                                const std::string& code,
                                const std::string& code_verifier)
{
    std::ostringstream out;
    out << "grant_type=authorization_code";
    out << "&client_id=" << auth::url_encode(oauth_config.client_id);
    out << "&code=" << auth::url_encode(code);
    out << "&redirect_uri=" << auth::url_encode(oauth_config.redirect_uri);
    out << "&code_verifier=" << auth::url_encode(code_verifier);
    return out.str();
}

std::string refresh_form(const auth::OAuthConfig& oauth_config, const std::string& refresh_token)
{
    std::ostringstream out;
    out << "grant_type=refresh_token";
    out << "&client_id=" << auth::url_encode(oauth_config.client_id);
    out << "&refresh_token=" << auth::url_encode(refresh_token);
    return out.str();
}

} // namespace

void LongbridgeClient::configure(ClientConfig config)
{
    config_ = std::move(config);
}

void LongbridgeClient::on_quote_delta(QuoteDeltaCallback callback)
{
    quote_delta_callback_ = std::move(callback);
}

void LongbridgeClient::on_state(ClientStateCallback callback)
{
    state_callback_ = std::move(callback);
}

ClientResult LongbridgeClient::fetch_socket_token(SocketToken& token_out)
{
    emit_state("fetching socket token");

#if defined(TAB5_HOST_TEST)
    (void)token_out;
    return { ClientError::UnsupportedCodec, 0, "ESP-IDF HTTP transport is not available in host tests" };
#else
    std::string body;
    int status = 0;
    constexpr const char* path = "/v1/socket/token";
    const auto result =
        perform_get(config_.endpoints.rest_base_url + path, path, config_.http_auth, body, status);
    if (!result.ok()) {
        return result;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return { ClientError::Json, status, "socket token response was not JSON" };
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* source = cJSON_IsObject(data) ? data : root;

    auto token = json_string(source, "socket_token");
    if (!token) {
        token = json_string(source, "token");
    }
    if (!token) {
        token = json_string(source, "otp");
    }

    auto socket_url = json_string(source, "socket_url");
    if (!socket_url) {
        socket_url = json_string(source, "url");
    }

    if (!token) {
        cJSON_Delete(root);
        return { ClientError::Json, status, "socket token response did not include a token" };
    }

    const std::string resolved_socket_url = socket_url.value_or(config_.endpoints.quote_ws_url);
    if (!is_allowed_quote_socket_url(resolved_socket_url, config_.endpoints)) {
        cJSON_Delete(root);
        return { ClientError::Protocol, status, "socket token response contained an unexpected quote socket URL" };
    }

    token_out.token = *token;
    token_out.socket_url = resolved_socket_url;
    token_out.expires_at_epoch_s = json_int64(source, "expires_at");
    cJSON_Delete(root);
    return {};
#endif
}

ClientResult LongbridgeClient::exchange_authorization_code(const auth::OAuthConfig& oauth_config,
                                                           const std::string& code,
                                                           const std::string& code_verifier,
                                                           auth::OAuthTokens& tokens_out)
{
    emit_state("exchanging oauth code");

#if defined(TAB5_HOST_TEST)
    (void)oauth_config;
    (void)code;
    (void)code_verifier;
    (void)tokens_out;
    return { ClientError::UnsupportedCodec, 0, "ESP-IDF HTTP transport is not available in host tests" };
#else
    std::string body;
    int status = 0;
    const auto result =
        perform_post_form(oauth_config.token_endpoint,
                          token_exchange_form(oauth_config, code, code_verifier),
                          body,
                          status);
    if (!result.ok()) {
        return result;
    }
    if (!parse_oauth_tokens(body, tokens_out)) {
        return { ClientError::Json, status, "OAuth token response did not contain tokens" };
    }
    return {};
#endif
}

ClientResult LongbridgeClient::refresh_access_token(const auth::OAuthConfig& oauth_config,
                                                    const std::string& refresh_token,
                                                    auth::OAuthTokens& tokens_out)
{
    emit_state("refreshing oauth token");

#if defined(TAB5_HOST_TEST)
    (void)oauth_config;
    (void)refresh_token;
    (void)tokens_out;
    return { ClientError::UnsupportedCodec, 0, "ESP-IDF HTTP transport is not available in host tests" };
#else
    std::string body;
    int status = 0;
    const auto result =
        perform_post_form(oauth_config.token_endpoint,
                          refresh_form(oauth_config, refresh_token),
                          body,
                          status);
    if (!result.ok()) {
        return result;
    }
    if (!parse_oauth_tokens(body, tokens_out)) {
        return { ClientError::Json, status, "OAuth refresh response did not contain tokens" };
    }
    return {};
#endif
}

ClientResult LongbridgeClient::fetch_quote_snapshots(
    const std::vector<quotes::SecuritySymbol>& symbols,
    std::vector<quotes::QuoteSnapshot>& snapshots_out)
{
    if (symbols.empty()) {
        snapshots_out.clear();
        return {};
    }

    emit_state("pulling quote snapshots");

#if defined(TAB5_HOST_TEST)
    snapshots_out.clear();
    return { ClientError::UnsupportedCodec, 0, "ESP-IDF WebSocket transport is not available in host tests" };
#else
    if (!quote_stream_connected()) {
        SocketToken socket_token;
        auto result = fetch_socket_token(socket_token);
        if (!result.ok()) {
            return result;
        }
        result = connect_quote_stream(socket_token);
        if (!result.ok()) {
            return result;
        }
    }

    std::vector<std::uint8_t> response_payload;
    const auto result = send_stream_request(CommandIntent::QuotePull,
                                            encode_multi_security_request(symbols),
                                            10000,
                                            &response_payload);
    if (!result.ok()) {
        return result;
    }

    auto decoded = decode_security_quote_response(response_payload.data(), response_payload.size());
    if (!decoded) {
        return { ClientError::Json, 0, "quote pull payload did not decode as SecurityQuoteResponse" };
    }

    snapshots_out = std::move(*decoded);
    if (snapshots_out.empty()) {
        return { ClientError::Json, 0, "quote pull response did not contain snapshots" };
    }
    return {};
#endif
}

ClientResult LongbridgeClient::connect_quote_stream(const SocketToken& socket_token)
{
    emit_state("connecting quote stream");

#if defined(TAB5_HOST_TEST)
    (void)socket_token;
    return { ClientError::UnsupportedCodec, 0, "ESP-IDF WebSocket transport is not available in host tests" };
#else
    if (socket_token.token.empty()) {
        return { ClientError::Protocol, 0, "socket token is empty" };
    }

    if (!ensure_stream_primitives()) {
        return { ClientError::Network, 0, "failed to allocate WebSocket synchronization primitives" };
    }

    disconnect();
    emit_state("connecting quote stream");

    const std::string base_url = socket_token.socket_url.empty()
        ? config_.endpoints.quote_ws_url
        : socket_token.socket_url;
    if (!is_allowed_quote_socket_url(base_url, config_.endpoints)) {
        return { ClientError::Protocol, 0, "quote socket URL did not match the configured Longbridge endpoint" };
    }
    stream_url_ = socket_url_with_handshake_query(base_url);

    esp_websocket_client_config_t ws_config {};
    ws_config.uri = stream_url_.c_str();
    ws_config.disable_auto_reconnect = true;
    ws_config.buffer_size = 8192;
    ws_config.network_timeout_ms = 10000;
    ws_config.ping_interval_sec = 15;
    ws_config.pingpong_timeout_sec = 10;
    ws_config.crt_bundle_attach = esp_crt_bundle_attach;
    ws_config.user_context = this;
    ws_config.task_stack = 8192;
    ws_config.task_name = "lb_quote_ws";

    websocket_client_ = esp_websocket_client_init(&ws_config);
    if (!websocket_client_) {
        return { ClientError::Network, 0, "failed to initialize quote WebSocket client" };
    }

    auto err = esp_websocket_register_events(websocket_client_,
                                             WEBSOCKET_EVENT_ANY,
                                             LongbridgeClient::websocket_event_handler,
                                             this);
    if (err != ESP_OK) {
        disconnect();
        return result_from_esp_error(err, "websocket event registration failed");
    }

    xEventGroupClearBits(stream_events_, kWsConnectedBit | kWsErrorBit | kWsResponseBit);
    err = esp_websocket_client_start(websocket_client_);
    if (err != ESP_OK) {
        disconnect();
        return result_from_esp_error(err, "websocket start failed");
    }

    const EventBits_t bits = xEventGroupWaitBits(stream_events_,
                                                 kWsConnectedBit | kWsErrorBit,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(15000));
    if ((bits & kWsConnectedBit) == 0) {
        disconnect();
        return { ClientError::Network, 0, "quote WebSocket connection timed out" };
    }

    emit_state("authenticating quote stream");
    auto auth_result = send_stream_request(CommandIntent::Auth, encode_auth_request(socket_token.token));
    if (!auth_result.ok()) {
        disconnect();
        return auth_result;
    }

    emit_state("quote stream authenticated");
    return {};
#endif
}

ClientResult LongbridgeClient::subscribe_quotes(const std::vector<quotes::SecuritySymbol>& symbols)
{
    if (symbols.empty()) {
        return {};
    }

    emit_state("subscribing quotes");

#if defined(TAB5_HOST_TEST)
    return { ClientError::UnsupportedCodec, 0, "ESP-IDF WebSocket transport is not available in host tests" };
#else
    if (!quote_stream_connected()) {
        return { ClientError::Network, 0, "quote stream is not connected" };
    }
    return send_stream_request(CommandIntent::QuoteSubscribe,
                               encode_subscribe_quote_request(symbols, true));
#endif
}

ClientResult LongbridgeClient::unsubscribe_quotes(const std::vector<quotes::SecuritySymbol>& symbols)
{
    if (symbols.empty()) {
        return {};
    }

    emit_state("unsubscribing quotes");

#if defined(TAB5_HOST_TEST)
    return { ClientError::UnsupportedCodec, 0, "ESP-IDF WebSocket transport is not available in host tests" };
#else
    if (!quote_stream_connected()) {
        return { ClientError::Network, 0, "quote stream is not connected" };
    }
    return send_stream_request(CommandIntent::QuoteUnsubscribe,
                               encode_unsubscribe_quote_request(symbols));
#endif
}

bool LongbridgeClient::quote_stream_connected() const
{
    return stream_connected_.load();
}

void LongbridgeClient::disconnect()
{
#if !defined(TAB5_HOST_TEST)
    stream_connected_ = false;
    pending_request_id_ = 0;
    if (stream_events_) {
        xEventGroupClearBits(stream_events_, kWsConnectedBit | kWsResponseBit);
    }
    if (websocket_client_) {
        esp_websocket_client_stop(websocket_client_);
        esp_websocket_client_destroy(websocket_client_);
        websocket_client_ = nullptr;
    }
    websocket_rx_.clear();
#endif
    emit_state("disconnected");
}

#if !defined(TAB5_HOST_TEST)
void LongbridgeClient::websocket_event_handler(void* handler_arg,
                                               esp_event_base_t event_base,
                                               std::int32_t event_id,
                                               void* event_data)
{
    auto* client = static_cast<LongbridgeClient*>(handler_arg);
    if (client) {
        client->handle_websocket_event(event_base, event_id, event_data);
    }
}

bool LongbridgeClient::ensure_stream_primitives()
{
    if (!stream_events_) {
        stream_events_ = xEventGroupCreate();
    }
    if (!stream_request_mutex_) {
        stream_request_mutex_ = xSemaphoreCreateMutex();
    }
    return stream_events_ && stream_request_mutex_;
}

ClientResult LongbridgeClient::send_stream_request(CommandIntent command,
                                                   std::vector<std::uint8_t> payload,
                                                   std::uint16_t timeout_ms,
                                                   std::vector<std::uint8_t>* response_payload_out)
{
    if (!ensure_stream_primitives()) {
        return { ClientError::Network, 0, "failed to allocate WebSocket synchronization primitives" };
    }
    if (!websocket_client_ || !stream_connected_.load()
        || !esp_websocket_client_is_connected(websocket_client_)) {
        return { ClientError::Network, 0, "quote WebSocket is not connected" };
    }

    if (xSemaphoreTake(stream_request_mutex_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return { ClientError::Protocol, 0, "timed out waiting for socket request slot" };
    }

    WireFrame frame;
    frame.packet_type = PacketType::Request;
    frame.command = command;
    frame.request_id = request_ids_.next();
    frame.timeout_ms = timeout_ms;
    frame.payload = std::move(payload);

    pending_request_id_ = frame.request_id;
    pending_status_code_ = 0xFFU;
    pending_response_payload_.clear();
    xEventGroupClearBits(stream_events_, kWsResponseBit | kWsErrorBit);

    const auto encoded = encode_frame(frame);
    const int sent = esp_websocket_client_send_bin(
        websocket_client_,
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<int>(encoded.size()),
        pdMS_TO_TICKS(timeout_ms));

    if (sent != static_cast<int>(encoded.size())) {
        pending_request_id_ = 0;
        xSemaphoreGive(stream_request_mutex_);
        return { ClientError::Network, 0, "failed to send full Longbridge socket request" };
    }

    const EventBits_t bits = xEventGroupWaitBits(stream_events_,
                                                 kWsResponseBit | kWsErrorBit,
                                                 pdTRUE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(timeout_ms));
    const std::uint8_t status = pending_status_code_.load();
    pending_request_id_ = 0;
    xSemaphoreGive(stream_request_mutex_);

    if ((bits & kWsResponseBit) != 0) {
        std::vector<std::uint8_t> response_payload = std::move(pending_response_payload_);
        pending_response_payload_.clear();
        if (response_payload_out) {
            *response_payload_out = response_payload;
        }
        return result_from_wire_status(status, command, response_payload);
    }
    if ((bits & kWsErrorBit) != 0) {
        pending_response_payload_.clear();
        return { ClientError::Network, 0, "quote WebSocket disconnected while waiting for response" };
    }
    pending_response_payload_.clear();
    return { ClientError::Protocol, 0, "Longbridge socket request timed out" };
}

void LongbridgeClient::handle_websocket_event(esp_event_base_t,
                                              std::int32_t event_id,
                                              void* event_data)
{
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        stream_connected_ = true;
        if (stream_events_) {
            xEventGroupSetBits(stream_events_, kWsConnectedBit);
        }
        emit_state("quote stream connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        stream_connected_ = false;
        if (stream_events_) {
            xEventGroupClearBits(stream_events_, kWsConnectedBit);
            xEventGroupSetBits(stream_events_, kWsErrorBit);
        }
        emit_state("quote stream disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        stream_connected_ = false;
        if (stream_events_) {
            xEventGroupSetBits(stream_events_, kWsErrorBit);
        }
        emit_state("quote stream error");
        if (data) {
            ESP_LOGW(kTag,
                     "websocket error type=%d tls=%s status=%d errno=%d",
                     static_cast<int>(data->error_handle.error_type),
                     esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
                     data->error_handle.esp_ws_handshake_status_code,
                     data->error_handle.esp_transport_sock_errno);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!data || !data->data_ptr || data->data_len <= 0) {
            break;
        }

        if (data->payload_offset == 0) {
            websocket_rx_.clear();
            websocket_rx_.reserve(static_cast<std::size_t>(data->payload_len));
        }
        websocket_rx_.insert(websocket_rx_.end(),
                             reinterpret_cast<const std::uint8_t*>(data->data_ptr),
                             reinterpret_cast<const std::uint8_t*>(data->data_ptr) + data->data_len);

        if (data->fin && static_cast<int>(websocket_rx_.size()) >= data->payload_len) {
            handle_websocket_message(websocket_rx_.data(), websocket_rx_.size());
            websocket_rx_.clear();
        }
        break;
    default:
        break;
    }
}

void LongbridgeClient::handle_websocket_message(const std::uint8_t* data, std::size_t length)
{
    auto decoded = decode_frame(data, length);
    if (decoded.status != DecodeStatus::Ok || !decoded.frame) {
        ESP_LOGW(kTag, "failed to decode Longbridge socket frame: %s", decode_status_text(decoded.status).c_str());
        if (stream_events_) {
            xEventGroupSetBits(stream_events_, kWsErrorBit);
        }
        return;
    }

    const WireFrame& frame = *decoded.frame;
    if (frame.packet_type == PacketType::Response) {
        if (frame.request_id == pending_request_id_.load()) {
            pending_status_code_ = frame.status_code;
            pending_response_payload_ = frame.payload;
            if (stream_events_) {
                xEventGroupSetBits(stream_events_, kWsResponseBit);
            }
        }
        return;
    }

    if (frame.packet_type == PacketType::Request && frame.command == CommandIntent::Heartbeat) {
        WireFrame response;
        response.packet_type = PacketType::Response;
        response.command = CommandIntent::Heartbeat;
        response.request_id = frame.request_id;
        response.status_code = 0;
        response.payload = frame.payload;
        const auto encoded = encode_frame(response);
        esp_websocket_client_send_bin(websocket_client_,
                                      reinterpret_cast<const char*>(encoded.data()),
                                      static_cast<int>(encoded.size()),
                                      pdMS_TO_TICKS(1000));
        return;
    }

    if (frame.packet_type == PacketType::Push && frame.command == CommandIntent::QuotePush) {
        auto delta = decode_push_quote(frame.payload.data(), frame.payload.size());
        if (!delta) {
            ESP_LOGW(kTag, "failed to decode Longbridge quote push payload");
            return;
        }
        delta->received_at_ms = esp_timer_get_time() / 1000;
        if (quote_delta_callback_) {
            quote_delta_callback_(*delta);
        }
    }
}
#endif

void LongbridgeClient::emit_state(const std::string& state)
{
    if (state_callback_) {
        state_callback_(state);
    }
}

const char* client_error_text(ClientError error)
{
    switch (error) {
    case ClientError::None:
        return "none";
    case ClientError::Network:
        return "network";
    case ClientError::Unauthorized:
        return "unauthorized";
    case ClientError::PermissionDenied:
        return "permission denied";
    case ClientError::RateLimited:
        return "rate limited";
    case ClientError::Protocol:
        return "protocol";
    case ClientError::Json:
        return "json";
    case ClientError::UnsupportedCodec:
        return "unsupported codec";
    }
    return "unknown";
}

} // namespace tab5::longbridge
