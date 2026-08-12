#pragma once

#include "auth/oauth.hpp"
#include "longbridge/api_auth.hpp"
#include "longbridge/endpoint.hpp"
#include "longbridge/protocol.hpp"
#include "quotes/quote_store.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <atomic>

#if !defined(TAB5_HOST_TEST)
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#endif

namespace tab5::longbridge {

enum class ClientError {
    None,
    Network,
    Unauthorized,
    PermissionDenied,
    RateLimited,
    Protocol,
    Json,
    UnsupportedCodec,
};

struct SocketToken {
    std::string token;
    std::string socket_url;
    std::int64_t expires_at_epoch_s { 0 };
};

struct ClientResult {
    ClientError error { ClientError::None };
    int http_status { 0 };
    std::string message;

    bool ok() const { return error == ClientError::None; }
};

using QuoteDeltaCallback = std::function<void(const quotes::QuoteDelta&)>;
using ClientStateCallback = std::function<void(const std::string&)>;

struct ClientConfig {
    EndpointSet endpoints;
    std::string access_token;
    HttpAuthConfig http_auth;
};

class LongbridgeClient {
public:
    LongbridgeClient() = default;

    void configure(ClientConfig config);
    void on_quote_delta(QuoteDeltaCallback callback);
    void on_state(ClientStateCallback callback);

    ClientResult fetch_socket_token(SocketToken& token_out);
    ClientResult exchange_authorization_code(const auth::OAuthConfig& oauth_config,
                                             const std::string& code,
                                             const std::string& code_verifier,
                                             auth::OAuthTokens& tokens_out);
    ClientResult refresh_access_token(const auth::OAuthConfig& oauth_config,
                                      const std::string& refresh_token,
                                      auth::OAuthTokens& tokens_out);
    ClientResult fetch_quote_snapshots(const std::vector<quotes::SecuritySymbol>& symbols,
                                       std::vector<quotes::QuoteSnapshot>& snapshots_out);
    ClientResult connect_quote_stream(const SocketToken& socket_token);
    ClientResult subscribe_quotes(const std::vector<quotes::SecuritySymbol>& symbols);
    ClientResult unsubscribe_quotes(const std::vector<quotes::SecuritySymbol>& symbols);
    bool quote_stream_connected() const;
    void disconnect();

private:
    ClientConfig config_;
    QuoteDeltaCallback quote_delta_callback_;
    ClientStateCallback state_callback_;
    RequestIdGenerator request_ids_;
    std::string stream_url_;
    std::vector<std::uint8_t> websocket_rx_;
    std::vector<std::uint8_t> pending_response_payload_;
    std::atomic<std::uint32_t> pending_request_id_ { 0 };
    std::atomic<std::uint8_t> pending_status_code_ { 0 };
    std::atomic<bool> stream_connected_ { false };

#if !defined(TAB5_HOST_TEST)
    esp_websocket_client_handle_t websocket_client_ { nullptr };
    EventGroupHandle_t stream_events_ { nullptr };
    SemaphoreHandle_t stream_request_mutex_ { nullptr };

    static void websocket_event_handler(void* handler_arg,
                                        esp_event_base_t event_base,
                                        std::int32_t event_id,
                                        void* event_data);
    bool ensure_stream_primitives();
    ClientResult send_stream_request(CommandIntent command,
                                     std::vector<std::uint8_t> payload,
                                     std::uint16_t timeout_ms = 5000,
                                     std::vector<std::uint8_t>* response_payload_out = nullptr);
    void handle_websocket_event(esp_event_base_t event_base,
                                std::int32_t event_id,
                                void* event_data);
    void handle_websocket_message(const std::uint8_t* data, std::size_t length);
#endif

    void emit_state(const std::string& state);
};

const char* client_error_text(ClientError error);

} // namespace tab5::longbridge
