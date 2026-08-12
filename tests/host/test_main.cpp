#include "auth/oauth.hpp"
#include "auth/oauth_callback_server.hpp"
#include "longbridge/api_auth.hpp"
#include "longbridge/endpoint.hpp"
#include "longbridge/protocol.hpp"
#include "longbridge/quote_codec.hpp"
#include "quotes/quote_store.hpp"
#include "quotes/symbol.hpp"
#include "settings/settings_file.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace tab5;

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}

void test_symbol_normalization()
{
    auto aapl = quotes::SecuritySymbol::parse(" aapl.us ");
    expect(aapl.has_value(), "AAPL.US parses");
    expect(aapl->value() == "AAPL.US", "AAPL.US normalizes uppercase");

    auto brk = quotes::SecuritySymbol::parse("brk.b.us");
    expect(brk.has_value(), "BRK.B.US parses using last region separator");
    expect(brk->value() == "BRK.B.US", "BRK.B.US preserves ticker dot");

    auto hk = quotes::SecuritySymbol::parse("700.hk");
    expect(hk.has_value(), "700.HK parses");
    expect(hk->value() == "700.HK", "700.HK normalizes uppercase");

    expect(!quotes::SecuritySymbol::parse("AAPL").has_value(), "missing region rejected");
    expect(!quotes::SecuritySymbol::parse("AAPL.LN").has_value(), "unsupported region rejected");
}

void test_watchlist()
{
    quotes::Watchlist watchlist;
    auto aapl = quotes::SecuritySymbol::parse("AAPL.US").value();
    expect(watchlist.add(aapl) == quotes::WatchlistAddResult::Added, "watchlist adds symbol");
    expect(watchlist.add(aapl) == quotes::WatchlistAddResult::AlreadyExists, "duplicate rejected");

    auto serialized = watchlist.serialize();
    auto restored = quotes::Watchlist::deserialize(serialized + "\nBAD.LN\n");
    expect(restored.size() == 1, "deserialize ignores invalid symbols");
    expect(restored.symbols().front().value() == "AAPL.US", "deserialize restores symbol");
}

void test_settings_file()
{
    const std::string content = R"CONF(
# Tab5 boot config
wifi_ssid = OfficeNet
wifi_password = pass phrase
endpoint = cn
client_id = lb-client-id
redirect_uri = http://tab5-stock.local/oauth/callback
watchlist = aapl.us, 700.hk
symbol = 600519.sh
access_token = access-1
refresh_token = refresh-1
token_expires_at = 1893456000
)CONF";

    auto parsed = settings::parse_settings_file(content);
    expect(parsed.ok(), "settings file parses complete config");
    expect(parsed.settings.wifi.ssid == "OfficeNet", "settings file parses Wi-Fi SSID");
    expect(parsed.settings.wifi.password == "pass phrase", "settings file parses Wi-Fi password");
    expect(parsed.settings.endpoint_region == longbridge::EndpointRegion::MainlandChina,
           "settings file parses endpoint");
    expect(parsed.settings.longbridge_client_id == "lb-client-id", "settings file parses client id");
    expect(parsed.settings.oauth_redirect_uri == "http://tab5-stock.local/oauth/callback",
           "settings file parses redirect URI");
    expect(parsed.settings.oauth_tokens.access_token == "access-1", "settings file parses access token");
    expect(parsed.settings.oauth_tokens.refresh_token == "refresh-1", "settings file parses refresh token");
    expect(parsed.settings.oauth_tokens.expires_at_epoch_s == 1893456000,
           "settings file parses token expiry");
    expect(parsed.settings.watchlist.size() == 3, "settings file appends watchlist symbols");
    expect(parsed.settings.watchlist.symbols()[0].value() == "AAPL.US", "settings file normalizes first symbol");
    expect(parsed.settings.watchlist.symbols()[1].value() == "700.HK", "settings file normalizes second symbol");
    expect(parsed.settings.watchlist.symbols()[2].value() == "600519.SH", "settings file normalizes symbol line");

    auto short_keys = settings::parse_settings_file(R"CONF(
ssid=Lab
password=
client_id=client
watchlist=000001.sz
)CONF");
    expect(short_keys.ok(), "settings file supports short keys and blank password");
    expect(short_keys.settings.endpoint_region == longbridge::EndpointRegion::Global,
           "settings file defaults endpoint to global");
    expect(short_keys.settings.watchlist.symbols().front().value() == "000001.SZ",
           "settings file parses short-key watchlist");

    auto missing = settings::parse_settings_file("wifi_ssid=Lab\nwatchlist=AAPL.US\n");
    expect(missing.status == settings::SettingsFileStatus::MissingRequired,
           "settings file rejects missing client id");

    auto invalid_watchlist = settings::parse_settings_file("wifi_ssid=Lab\nclient_id=client\nwatchlist=BAD.LN\n");
    expect(invalid_watchlist.status == settings::SettingsFileStatus::InvalidWatchlist,
           "settings file rejects empty normalized watchlist");

    auto invalid_endpoint =
        settings::parse_settings_file("wifi_ssid=Lab\nclient_id=client\nendpoint=eu\nwatchlist=AAPL.US\n");
    expect(invalid_endpoint.status == settings::SettingsFileStatus::InvalidEndpoint,
           "settings file rejects invalid endpoint");

    auto invalid_expiry =
        settings::parse_settings_file("wifi_ssid=Lab\nclient_id=client\ntoken_expires_at=\nwatchlist=AAPL.US\n");
    expect(invalid_expiry.ok(), "settings file still imports config with blank token expiry");
    expect(!invalid_expiry.warnings.empty(), "settings file warns on blank token expiry");

    auto empty = settings::parse_settings_file("# comment only\n\n");
    expect(empty.status == settings::SettingsFileStatus::Empty, "settings file reports empty content");

    auto api_key = settings::parse_settings_file(R"CONF(
ssid=Lab
auth_mode=api_key
app_key=ap-app-key
app_secret=ap-app-secret
access_token=ap-access-token
watchlist=AAPL.US
)CONF");
    expect(api_key.ok(), "settings file parses API key auth config");
    expect(api_key.settings.auth_mode == settings::AuthMode::ApiKey, "settings file selects API key mode");
    expect(api_key.settings.api_key.app_key == "ap-app-key", "settings file parses API app key");
    expect(api_key.settings.api_key.app_secret == "ap-app-secret", "settings file parses API app secret");
    expect(api_key.settings.api_key.access_token == "ap-access-token",
           "settings file maps access_token to API key mode");
    expect(api_key.settings.oauth_tokens.empty(), "settings file does not treat API access_token as OAuth token");

    auto incomplete_api_key = settings::parse_settings_file(R"CONF(
ssid=Lab
auth_mode=api_key
app_key=key
watchlist=AAPL.US
)CONF");
    expect(incomplete_api_key.status == settings::SettingsFileStatus::MissingRequired,
           "settings file rejects incomplete API key credentials");
}

void test_api_key_auth_headers()
{
    const longbridge::HttpAuthConfig api_auth {
        longbridge::HttpAuthMode::ApiKey,
        "",
        {
            "test_app_key",
            "test_app_secret",
            "test_access_token",
        },
    };
    const auto headers = longbridge::build_longbridge_auth_headers(api_auth,
                                                                   "GET",
                                                                   "/v1/socket/token",
                                                                   "",
                                                                   "",
                                                                   1700000000000LL);
    auto header_value = [&headers](const std::string& name) {
        for (const auto& header : headers) {
            if (header.name == name) {
                return header.value;
            }
        }
        return std::string {};
    };

    expect(header_value("authorization") == "test_access_token", "API key auth strips bearer prefix");
    expect(header_value("x-api-key") == "test_app_key", "API key auth sends app key");
    expect(header_value("x-dc-region") == "ap", "API key auth defaults to AP data center");
    expect(header_value("x-timestamp") == "1700000000000", "API key auth sends timestamp");
    expect(header_value("x-api-signature")
               == "HMAC-SHA256 SignedHeaders=authorization;x-api-key;x-timestamp, Signature="
                  "6ee4f57c0b56dab539055910d2d41acb5358e07d977034c85ec9acb02ce44a5c",
           "API key auth matches Longbridge SDK signature format");

    const longbridge::HttpAuthConfig us_auth {
        longbridge::HttpAuthMode::ApiKey,
        "",
        {
            "us_app_key",
            "ap_secret",
            "Bearer ap_access_token",
        },
    };
    const auto us_headers =
        longbridge::build_longbridge_auth_headers(us_auth, "GET", "/v1/socket/token", "", "", 1700000000000LL);
    bool saw_us_region = false;
    bool saw_stripped_token = false;
    for (const auto& header : us_headers) {
        if (header.name == "x-dc-region" && header.value == "us") {
            saw_us_region = true;
        }
        if (header.name == "authorization" && header.value == "ap_access_token") {
            saw_stripped_token = true;
        }
    }
    expect(saw_us_region, "API key auth derives US data center from credential prefix");
    expect(saw_stripped_token, "API key auth tolerates Bearer-prefixed access token");
}

void test_quote_store()
{
    auto aapl = quotes::SecuritySymbol::parse("AAPL.US").value();
    quotes::Watchlist watchlist;
    watchlist.add(aapl);

    quotes::QuoteStore store;
    store.set_watchlist(watchlist);

    quotes::QuoteSnapshot snapshot;
    snapshot.symbol = aapl;
    snapshot.last_price = 101.0;
    snapshot.previous_close = 100.0;
    snapshot.open = 98.0;
    snapshot.timestamp_ms = 1000;
    snapshot.received_at_ms = 1000;
    store.apply_snapshot(snapshot);

    auto stored = store.get(aapl);
    expect(stored.has_value(), "snapshot stored");
    expect(std::fabs(stored->change().value() - 1.0) < 0.0001, "change computed");
    expect(std::fabs(stored->change_percent().value() - 1.0) < 0.0001, "change percent computed");

    quotes::QuoteDelta delta;
    delta.symbol = aapl;
    delta.last_price = 105.0;
    delta.timestamp_ms = 2000;
    delta.received_at_ms = 2000;
    delta.sequence = 10;
    expect(store.apply_delta(delta), "newer delta applies");

    stored = store.get(aapl);
    expect(stored->last_price.value() == 105.0, "delta changes last price");
    expect(stored->open.value() == 98.0, "delta does not blank unchanged fields");

    quotes::QuoteSnapshot refreshed_snapshot;
    refreshed_snapshot.symbol = aapl;
    refreshed_snapshot.last_price = 106.0;
    refreshed_snapshot.previous_close = 100.0;
    refreshed_snapshot.timestamp_ms = 4000;
    refreshed_snapshot.received_at_ms = 4000;
    store.apply_snapshot(refreshed_snapshot);
    expect(store.get(aapl)->sequence == 10, "snapshot refresh preserves push sequence watermark");

    quotes::QuoteDelta old_delta;
    old_delta.symbol = aapl;
    old_delta.last_price = 88.0;
    old_delta.timestamp_ms = 5000;
    old_delta.received_at_ms = 5000;
    old_delta.sequence = 9;
    expect(!store.apply_delta(old_delta), "older delta rejected");
    expect(store.get(aapl)->last_price.value() == 106.0, "older delta does not mutate snapshot");

    expect(!store.mark_stale_older_than(8000, 5000), "fresh snapshot does not change stale state");
    expect(store.mark_stale_older_than(10000, 5000), "stale transition is reported");
    expect(store.get(aapl)->stale, "old snapshot marked stale");

    quotes::QuoteDelta first;
    first.symbol = aapl;
    first.last_price = 110.0;
    first.open = 108.0;
    quotes::QuoteDelta second;
    second.symbol = aapl;
    second.high = 111.0;
    second.sequence = 12;
    const auto merged = quotes::merge_delta(first, second);
    expect(merged.last_price.value() == 110.0, "merged delta keeps old sparse field");
    expect(merged.high.value() == 111.0, "merged delta overlays new sparse field");
    expect(merged.sequence.value() == 12, "merged delta overlays sequence");

    quotes::QuoteDelta older;
    older.symbol = aapl;
    older.last_price = 99.0;
    older.sequence = 11;
    const auto still_newer = quotes::merge_delta(merged, older);
    expect(still_newer.last_price.value() == 110.0, "older pending delta does not overwrite price");
    expect(still_newer.high.value() == 111.0, "older pending delta does not erase newer sparse field");
    expect(still_newer.sequence.value() == 12, "older pending delta does not lower sequence");
}

void test_oauth_pkce()
{
    const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const std::string expected = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    expect(auth::pkce_challenge_for_verifier(verifier) == expected, "RFC7636 PKCE vector");

    auth::OAuthConfig config {
        "https://openapi.longbridge.com/oauth2/authorize",
        "https://openapi.longbridge.com/oauth2/token",
        "client id",
        "http://tab5.local/oauth/callback",
        "3",
    };
    const auto url = auth::build_authorization_url(config, "state 1", "challenge");
    expect(url.find("response_type=code") != std::string::npos, "authorization URL response type");
    expect(url.find("client_id=client%20id") != std::string::npos, "authorization URL encodes client_id");
    expect(url.find("code_challenge_method=S256") != std::string::npos, "authorization URL uses S256");

    const auto callback =
        auth::parse_oauth_callback_query("?code=abc%20123&state=state+1");
    expect(callback.has_code(), "callback code parsed");
    expect(callback.code == "abc 123", "callback code URL decoded");
    expect(callback.state == "state 1", "callback state URL decoded");

    const auto denied = auth::parse_oauth_callback_query("?error=access_denied&state=state");
    expect(!denied.has_code(), "callback with error rejected as code-bearing callback");

    auth::OAuthCallbackServer server;
    int accepted = 0;
    expect(server.start(80,
                        "state 1",
                        [&accepted](const auth::OAuthCallbackParams&) {
                            ++accepted;
                        }),
           "host callback server starts logical callback gate");
    expect(!server.handle_query("?code=abc&state=wrong"), "wrong callback state rejected");
    expect(server.handle_query("?code=abc&state=state+1"), "right callback accepted");
    expect(!server.handle_query("?code=abc&state=state+1"), "callback is one-shot");
    expect(accepted == 1, "callback handler called once");
}

void test_protocol_frame()
{
    longbridge::WireFrame frame;
    frame.command = longbridge::CommandIntent::QuoteSubscribe;
    frame.timeout_ms = 5000;
    frame.request_id = 42;
    frame.payload = { 1, 2, 3, 4 };

    const auto handshake = longbridge::encode_handshake();
    expect(handshake.size() == longbridge::kHandshakeLength, "handshake length");
    expect(handshake[0] == 0x11, "handshake uses protocol v1 protobuf");

    const auto encoded = longbridge::encode_frame(frame);
    expect(encoded.size() == longbridge::kRequestHeaderLength + 4, "frame includes request header and payload");
    expect(encoded[0] == 0x01, "request frame type byte");
    expect(encoded[1] == 0x06, "request command byte");
    expect(encoded[2] == 0x00 && encoded[3] == 0x00 && encoded[4] == 0x00 && encoded[5] == 0x2A,
           "request id is big-endian after command");
    expect(encoded[6] == 0x13 && encoded[7] == 0x88, "request timeout is big-endian");
    expect(encoded[8] == 0x00 && encoded[9] == 0x00 && encoded[10] == 0x04,
           "request payload length is big-endian u24");

    const auto decoded = longbridge::decode_frame(encoded.data(), encoded.size());
    expect(decoded.status == longbridge::DecodeStatus::Ok, "frame decodes");
    expect(decoded.consumed == encoded.size(), "decode reports consumed bytes");
    expect(decoded.frame->packet_type == longbridge::PacketType::Request, "packet type round trips");
    expect(decoded.frame->command == longbridge::CommandIntent::QuoteSubscribe, "command round trips");
    expect(decoded.frame->request_id == 42, "request id round trips");
    expect(decoded.frame->payload == frame.payload, "payload round trips");

    auto truncated = longbridge::decode_frame(encoded.data(), encoded.size() - 1);
    expect(truncated.status == longbridge::DecodeStatus::Incomplete, "truncated frame waits for more bytes");

    auto bad = encoded;
    bad[0] = 0;
    expect(longbridge::decode_frame(bad.data(), bad.size()).status == longbridge::DecodeStatus::BadMagic,
           "bad packet type rejected");

    longbridge::WireFrame push;
    push.packet_type = longbridge::PacketType::Push;
    push.command = longbridge::CommandIntent::QuotePush;
    push.payload = { 9, 8 };
    const auto encoded_push = longbridge::encode_frame(push);
    expect(encoded_push.size() == longbridge::kPushHeaderLength + 2, "push frame length");
    expect(encoded_push[0] == 0x03 && encoded_push[1] == 0x65, "push header starts with type and command");
    expect(encoded_push[2] == 0x00 && encoded_push[3] == 0x00 && encoded_push[4] == 0x02,
           "push payload length follows command");
    expect(longbridge::decode_frame(encoded_push.data(), encoded_push.size()).frame->command
               == longbridge::CommandIntent::QuotePush,
           "push command decodes");

    longbridge::BackoffPolicy backoff(100, 350, 2);
    expect(backoff.next_delay_ms() == 100, "backoff starts at base");
    expect(backoff.next_delay_ms() == 200, "backoff doubles");
    expect(backoff.next_delay_ms() == 350, "backoff caps at max");
}

void test_longbridge_endpoints()
{
    const auto global = longbridge::default_endpoints(longbridge::EndpointRegion::Global);
    expect(global.rest_base_url == "https://openapi.longbridge.com", "global REST endpoint");
    expect(global.quote_ws_url == "wss://openapi-quote.longbridge.com", "global quote WebSocket endpoint");
    expect(global.authorize_url == "https://openapi.longbridge.com/oauth2/authorize", "global OAuth authorize endpoint");
    expect(global.token_url == "https://openapi.longbridge.com/oauth2/token", "global OAuth token endpoint");

    const auto cn = longbridge::default_endpoints(longbridge::EndpointRegion::MainlandChina);
    expect(cn.rest_base_url == "https://openapi.longbridge.cn", "cn REST endpoint");
    expect(cn.quote_ws_url == "wss://openapi-quote.longbridge.cn", "cn quote WebSocket endpoint");
}

void test_quote_codec()
{
    auto aapl = quotes::SecuritySymbol::parse("AAPL.US").value();
    std::vector<quotes::SecuritySymbol> symbols { aapl };
    const auto quote_pull = longbridge::encode_multi_security_request(symbols);
    const std::vector<std::uint8_t> expected_quote_pull = {
        0x0A, 0x07, 'A', 'A', 'P', 'L', '.', 'U', 'S',
    };
    expect(quote_pull == expected_quote_pull, "quote pull protobuf payload encoded");

    const auto subscribe = longbridge::encode_subscribe_quote_request(symbols, true);
    const std::vector<std::uint8_t> expected_subscribe = {
        0x0A, 0x07, 'A', 'A', 'P', 'L', '.', 'U', 'S',
        0x10, 0x01,
        0x18, 0x01,
    };
    expect(subscribe == expected_subscribe, "subscribe protobuf payload encoded");

    const auto unsubscribe = longbridge::encode_unsubscribe_quote_request(symbols);
    const std::vector<std::uint8_t> expected_unsubscribe = {
        0x0A, 0x07, 'A', 'A', 'P', 'L', '.', 'U', 'S',
        0x10, 0x01,
        0x18, 0x00,
    };
    expect(unsubscribe == expected_unsubscribe, "unsubscribe protobuf payload encoded");

    const auto unsubscribe_all = longbridge::encode_unsubscribe_quote_request({}, true);
    const std::vector<std::uint8_t> expected_unsubscribe_all = {
        0x10, 0x01,
        0x18, 0x01,
    };
    expect(unsubscribe_all == expected_unsubscribe_all, "unsubscribe-all protobuf payload encoded");

    const auto auth = longbridge::encode_auth_request("otp-token");
    const std::vector<std::uint8_t> expected_auth = {
        0x0A, 0x09, 'o', 't', 'p', '-', 't', 'o', 'k', 'e', 'n',
    };
    expect(auth == expected_auth, "auth protobuf payload encoded");

    const std::vector<std::uint8_t> quote_response = {
        0x0A, 0x42,
        0x0A, 0x07, 'A', 'A', 'P', 'L', '.', 'U', 'S',
        0x12, 0x06, '1', '8', '9', '.', '1', '0',
        0x1A, 0x06, '1', '8', '8', '.', '0', '0',
        0x22, 0x06, '1', '8', '8', '.', '5', '0',
        0x2A, 0x06, '1', '9', '0', '.', '0', '0',
        0x32, 0x06, '1', '8', '7', '.', '5', '0',
        0x38, 0xE8, 0x0F,
        0x40, 0xC0, 0x84, 0x3D,
        0x4A, 0x06, '7', '5', '6', '0', '0', '0',
        0x50, 0x00,
    };
    auto snapshots = longbridge::decode_security_quote_response(quote_response.data(), quote_response.size());
    expect(snapshots.has_value(), "security quote response decodes");
    expect(snapshots->size() == 1, "security quote response has one row");
    expect(snapshots->front().symbol.value() == "AAPL.US", "security quote symbol decoded");
    expect(snapshots->front().last_price.value() == 189.10, "security quote last price decoded");
    expect(snapshots->front().previous_close.value() == 188.00, "security quote prev close decoded");
    expect(snapshots->front().open.value() == 188.50, "security quote open decoded");
    expect(snapshots->front().high.value() == 190.00, "security quote high decoded");
    expect(snapshots->front().low.value() == 187.50, "security quote low decoded");
    expect(snapshots->front().timestamp_ms == 2024, "security quote timestamp decoded");
    expect(snapshots->front().volume.value() == 1000000.0, "security quote volume decoded");
    expect(snapshots->front().turnover.value() == 756000.0, "security quote turnover decoded");
    expect(snapshots->front().trade_status == "NORMAL", "security quote trade status decoded");

    const std::vector<std::uint8_t> push = {
        0x0A, 0x07, 'A', 'A', 'P', 'L', '.', 'U', 'S',
        0x10, 0x7B,
        0x1A, 0x06, '1', '8', '9', '.', '1', '0',
        0x22, 0x06, '1', '8', '8', '.', '0', '0',
        0x2A, 0x06, '1', '9', '0', '.', '0', '0',
        0x32, 0x06, '1', '8', '7', '.', '5', '0',
        0x38, 0xE8, 0x0F,
        0x40, 0xC0, 0x84, 0x3D,
        0x4A, 0x06, '7', '5', '6', '0', '0', '0',
        0x50, 0x00,
        0x58, 0x00,
    };
    auto delta = longbridge::decode_push_quote(push.data(), push.size());
    expect(delta.has_value(), "push quote protobuf decodes");
    expect(delta->symbol.value() == "AAPL.US", "push symbol decoded");
    expect(delta->sequence.value() == 123, "push sequence decoded");
    expect(delta->last_price.value() == 189.10, "push last price decoded");
    expect(delta->open.value() == 188.00, "push open decoded");
    expect(delta->high.value() == 190.00, "push high decoded");
    expect(delta->low.value() == 187.50, "push low decoded");
    expect(delta->timestamp_ms.value() == 2024, "push timestamp decoded");
    expect(delta->volume.value() == 1000000.0, "push volume decoded");
    expect(delta->turnover.value() == 756000.0, "push turnover decoded");
    expect(delta->trade_status.value() == "NORMAL", "push trade status decoded");
    expect(delta->session.value() == "NORMAL_TRADE", "push trade session decoded");

    const std::vector<std::uint8_t> truncated_string = { 0x0A, 0x08, 'A', 'A', 'P', 'L' };
    expect(!longbridge::decode_security_quote_response(truncated_string.data(), truncated_string.size()).has_value(),
           "truncated security quote response rejected");
    expect(!longbridge::decode_push_quote(truncated_string.data(), truncated_string.size()).has_value(),
           "truncated string field rejected");
    const std::vector<std::uint8_t> unsupported_wire_type = { 0x0D, 0x00, 0x00, 0x00, 0x00 };
    expect(!longbridge::decode_push_quote(unsupported_wire_type.data(), unsupported_wire_type.size()).has_value(),
           "unsupported wire type rejected");
}

} // namespace

int main()
{
    test_symbol_normalization();
    test_watchlist();
    test_settings_file();
    test_api_key_auth_headers();
    test_quote_store();
    test_oauth_pkce();
    test_protocol_frame();
    test_longbridge_endpoints();
    test_quote_codec();
    std::cout << "host tests passed\n";
    return 0;
}
