#include "sd_file_manager/sd_file_manager.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#if !defined(TAB5_HOST_TEST)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"
#endif
#endif

namespace tab5::sd_file_manager {
namespace {

constexpr char kPathSeparator = '/';
constexpr const char* kRootPath = "/";

enum class RouteId {
    Edit,
    Create,
    Mkdir,
    Save,
    DeleteForm,
    ApiFilePut,
    ApiFileDelete,
    Files,
};

struct InternalRouteSpec {
    RouteId id;
    const char* uri;
    RouteMethod method;
};

constexpr std::array<InternalRouteSpec, 8> kRoutes {{
    { RouteId::Edit, "/edit", RouteMethod::Get },
    { RouteId::Create, "/create", RouteMethod::Post },
    { RouteId::Mkdir, "/mkdir", RouteMethod::Post },
    { RouteId::Save, "/save", RouteMethod::Post },
    { RouteId::DeleteForm, "/delete", RouteMethod::Post },
    { RouteId::ApiFilePut, "/api/file", RouteMethod::Put },
    { RouteId::ApiFileDelete, "/api/file", RouteMethod::Delete },
    { RouteId::Files, "/*", RouteMethod::Get },
}};

bool is_unreserved_url_char(unsigned char ch)
{
    return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool is_invalid_fat_path_char(unsigned char ch)
{
    switch (ch) {
    case '\0':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
        return true;
    default:
        return ch < 0x20 || ch == 0x7f;
    }
}

#if !defined(TAB5_HOST_TEST)

constexpr const char* kTag = "sd-file-web";
constexpr std::size_t kScratchBytes = 2048;
constexpr std::size_t kMaxFormBodyBytes = kMaxEditableFileBytes * 3 + kMaxPathBytes + 128;
constexpr unsigned kMaxReceiveTimeouts = 8;

struct ServerContext {
    SemaphoreHandle_t sd_mutex { nullptr };
    std::string access_token;
};

struct DirectoryEntry {
    std::string name;
    std::string path;
    bool directory { false };
    std::uint64_t size { 0 };
};

enum class BodyReadError {
    None,
    TooLarge,
    Timeout,
    ReceiveFailed,
};

std::string full_sd_path(std::string_view normalized_path)
{
    std::string out = BSP_SD_MOUNT_POINT;
    if (normalized_path != kRootPath) {
        out.append(normalized_path);
    }
    return out;
}

void send_text(httpd_req_t* request, const char* status, const char* type, const std::string& body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, type);
    httpd_resp_send(request, body.data(), body.size());
}

void send_body_read_error(httpd_req_t* request, BodyReadError error)
{
    switch (error) {
    case BodyReadError::TooLarge:
        httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "Request body is too large");
        break;
    case BodyReadError::Timeout:
        httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT, "Timed out receiving request body");
        break;
    case BodyReadError::ReceiveFailed:
    case BodyReadError::None:
    default:
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive request body");
        break;
    }
}

void send_redirect_to(httpd_req_t* request, const std::string& location)
{
    const std::string safe_location = location.empty() ? "/" : location;
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", safe_location.c_str());
    httpd_resp_sendstr(request, "Redirecting");
}

bool get_query_value(httpd_req_t* request,
                     const char* key,
                     char* value,
                     std::size_t value_size)
{
    std::array<char, 768> query {};
    const esp_err_t query_err = httpd_req_get_url_query_str(request, query.data(), query.size());
    if (query_err != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query.data(), key, value, value_size) == ESP_OK;
}

bool constant_time_equal(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }

    unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

bool token_matches(const ServerContext& context, std::string_view token)
{
    return !context.access_token.empty() && constant_time_equal(token, context.access_token);
}

bool has_valid_access_token(httpd_req_t* request, const ServerContext& context)
{
    std::array<char, 96> header {};
    if (httpd_req_get_hdr_value_str(request,
                                    "X-Tab5-SD-Token",
                                    header.data(),
                                    header.size())
        == ESP_OK) {
        if (token_matches(context, header.data())) {
            return true;
        }
    }

    std::array<char, 96> query_token {};
    if (get_query_value(request, "token", query_token.data(), query_token.size())) {
        if (token_matches(context, query_token.data())) {
            return true;
        }
    }
    return false;
}

bool authorize_or_send_error(httpd_req_t* request, const ServerContext& context)
{
    if (has_valid_access_token(request, context)) {
        return true;
    }
    httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer realm=\"tab5-sd\"");
    httpd_resp_send_err(request, HTTPD_401_UNAUTHORIZED, "Missing or invalid SD file manager token");
    return false;
}

PathResult path_from_query(httpd_req_t* request, const char* key)
{
    std::array<char, kMaxPathBytes * 3 + 1> raw {};
    if (!get_query_value(request, key, raw.data(), raw.size())) {
        return { {}, PathError::Empty };
    }
    auto decoded = url_decode(raw.data());
    if (!decoded.has_value()) {
        return { {}, PathError::InvalidCharacter };
    }
    return normalize_request_path(*decoded);
}

PathResult path_from_uri(httpd_req_t* request)
{
    std::string uri = request->uri;
    const auto query = uri.find('?');
    if (query != std::string::npos) {
        uri.resize(query);
    }
    if (uri.empty()) {
        uri = "/";
    }
    auto decoded = url_decode(uri, false);
    if (!decoded.has_value()) {
        return { {}, PathError::InvalidCharacter };
    }
    return normalize_request_path(*decoded);
}

class SdCardSession {
public:
    explicit SdCardSession(ServerContext& context)
        : context_(context)
    {
        if (context_.sd_mutex) {
            locked_ = xSemaphoreTake(context_.sd_mutex, pdMS_TO_TICKS(10000)) == pdTRUE;
        }
        if (!locked_) {
            return;
        }

        const esp_err_t err = bsp_sdcard_mount();
        if (err == ESP_OK) {
            mounted_for_session_ = true;
            ok_ = true;
            return;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            ok_ = true;
            return;
        }
        error_ = err;
    }

    ~SdCardSession()
    {
        if (mounted_for_session_) {
            const esp_err_t err = bsp_sdcard_unmount();
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "SD card unmount failed: %s", esp_err_to_name(err));
            }
        }
        if (locked_) {
            xSemaphoreGive(context_.sd_mutex);
        }
    }

    bool ok() const { return ok_; }
    esp_err_t error() const { return error_; }

private:
    ServerContext& context_;
    bool locked_ { false };
    bool mounted_for_session_ { false };
    bool ok_ { false };
    esp_err_t error_ { ESP_ERR_TIMEOUT };
};

std::unique_ptr<SdCardSession> begin_sd_or_send_error(httpd_req_t* request, ServerContext& context)
{
    auto session = new SdCardSession(context);
    if (!session) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return nullptr;
    }
    if (!session->ok()) {
        const std::string message = std::string("SD card unavailable: ") + esp_err_to_name(session->error());
        delete session;
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, message.c_str());
        return nullptr;
    }
    return std::unique_ptr<SdCardSession>(session);
}

std::string hex_from_bytes(const std::uint8_t* bytes, std::size_t byte_count)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(byte_count * 2);
    for (std::size_t i = 0; i < byte_count; ++i) {
        const auto byte = bytes[i];
        output.push_back(kHex[(byte >> 4U) & 0x0fU]);
        output.push_back(kHex[byte & 0x0fU]);
    }
    return output;
}

std::string random_hex_bytes(std::size_t byte_count)
{
    std::array<std::uint8_t, 16> bytes {};
    const std::size_t fill = std::min(byte_count, bytes.size());
    esp_fill_random(bytes.data(), fill);
    return hex_from_bytes(bytes.data(), fill);
}

std::string generate_access_token()
{
    return random_hex_bytes(16);
}

std::string make_root_temp_path(const char* purpose)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string path = BSP_SD_MOUNT_POINT;
        path += "/.tab5-";
        path += purpose;
        path += "-";
        path += random_hex_bytes(8);
        path += ".tmp";

        struct stat st {};
        if (stat(path.c_str(), &st) != 0) {
            return path;
        }
    }
    return {};
}

bool close_written_file(FILE* file)
{
    return std::fclose(file) == 0;
}

esp_err_t replace_file_with_temp(httpd_req_t* request,
                                 const std::string& temp_path,
                                 const std::string& full_path)
{
    struct stat st {};
    const bool target_exists = stat(full_path.c_str(), &st) == 0;
    if (target_exists && S_ISDIR(st.st_mode)) {
        std::remove(temp_path.c_str());
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Path is a directory");
        return ESP_FAIL;
    }

    bool moved_target_to_backup = false;
    std::string backup_path;
    if (target_exists) {
        backup_path = make_root_temp_path("backup");
        if (backup_path.empty()) {
            std::remove(temp_path.c_str());
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reserve backup path");
            return ESP_FAIL;
        }
        if (rename(full_path.c_str(), backup_path.c_str()) != 0) {
            std::remove(temp_path.c_str());
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to prepare replacement");
            return ESP_FAIL;
        }
        moved_target_to_backup = true;
    }

    if (rename(temp_path.c_str(), full_path.c_str()) != 0) {
        const int replace_errno = errno;
        if (moved_target_to_backup && rename(backup_path.c_str(), full_path.c_str()) != 0) {
            ESP_LOGE(kTag,
                     "failed restoring original file after replacement error for %s",
                     full_path.c_str());
        }
        std::remove(temp_path.c_str());
        ESP_LOGW(kTag, "failed replacing %s: errno=%d", full_path.c_str(), replace_errno);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to replace file");
        return ESP_FAIL;
    }

    if (moved_target_to_backup && std::remove(backup_path.c_str()) != 0) {
        ESP_LOGW(kTag, "failed removing backup file %s", backup_path.c_str());
    }
    return ESP_OK;
}

void append_chunk(httpd_req_t* request, const std::string& text)
{
    httpd_resp_send_chunk(request, text.data(), text.size());
}

void append_header(httpd_req_t* request, const std::string& title)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    append_chunk(request,
                 "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                 "<title>");
    append_chunk(request, html_escape(title));
    append_chunk(request,
                 "</title><style>"
                 ":root{color-scheme:light;--bg:#f6f7f9;--ink:#15171a;--muted:#667085;"
                 "--line:#d9dee7;--panel:#fff;--accent:#0b6bcb;--danger:#b42318}"
                 "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);"
                 "font:14px/1.45 -apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif}"
                 "header{display:flex;align-items:center;justify-content:space-between;gap:12px;"
                 "padding:14px 18px;border-bottom:1px solid var(--line);background:var(--panel)}"
                 "h1{margin:0;font-size:18px;font-weight:650}.muted{color:var(--muted)}"
                 "main{display:grid;grid-template-columns:minmax(260px,360px) 1fr;min-height:calc(100vh - 57px)}"
                 "aside{padding:16px;border-right:1px solid var(--line);background:var(--panel)}"
                 "section{padding:16px}.crumbs{margin:0 0 12px;color:var(--muted);word-break:break-all}"
                 "table{width:100%;border-collapse:collapse;background:var(--panel)}"
                 "th,td{border-bottom:1px solid var(--line);padding:8px;text-align:left;vertical-align:middle}"
                 "th{font-size:12px;color:var(--muted);font-weight:600}a{color:var(--accent);text-decoration:none}"
                 "a:hover{text-decoration:underline}.actions{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
                 "form{margin:0}.stack{display:grid;gap:10px}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
                 "input,textarea{width:100%;border:1px solid var(--line);border-radius:6px;padding:8px;"
                 "font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;background:#fff}"
                 "textarea{min-height:60vh;resize:vertical}button,.button{border:1px solid var(--line);"
                 "border-radius:6px;background:#fff;color:var(--ink);padding:7px 10px;cursor:pointer;display:inline-block}"
                 "button.primary{background:var(--accent);border-color:var(--accent);color:#fff}"
                 "button.danger{color:var(--danger)}button:focus,input:focus,textarea:focus,.button:focus{outline:2px solid #84c5ff;outline-offset:1px}"
                 ".notice{padding:10px 12px;border:1px solid var(--line);background:#fff;border-radius:6px}"
                 "@media(max-width:760px){main{grid-template-columns:1fr}aside{border-right:0;border-bottom:1px solid var(--line)}textarea{min-height:45vh}}"
                 "</style></head><body><header><h1>Tab5 SD 文件</h1><span class=\"muted\">/sdcard</span></header><main>");
}

void append_footer(httpd_req_t* request)
{
    append_chunk(request, "</main></body></html>");
    httpd_resp_send_chunk(request, nullptr, 0);
}

std::string with_access_token(std::string href, const ServerContext& context)
{
    href += href.find('?') == std::string::npos ? '?' : '&';
    href += "token=";
    href += url_encode(context.access_token);
    return href;
}

std::string make_file_href(const std::string& path, const ServerContext& context)
{
    return with_access_token(url_encode(path), context);
}

std::string make_editor_href(const std::string& path, const ServerContext& context)
{
    return with_access_token("/edit?path=" + url_encode(path), context);
}

std::string format_size(std::uint64_t size)
{
    char buffer[32] {};
    if (size < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(size));
    } else if (size < 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(size) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(size) / (1024.0 * 1024.0));
    }
    return buffer;
}

std::vector<DirectoryEntry> read_directory(const std::string& normalized_path)
{
    std::vector<DirectoryEntry> entries;
    const std::string dir_path = full_sd_path(normalized_path);
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        return entries;
    }

    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }

        std::string child = normalized_path;
        if (child != kRootPath) {
            child += "/";
        }
        child += name;

        const std::string full_path = full_sd_path(child);
        struct stat st {};
        if (stat(full_path.c_str(), &st) != 0) {
            continue;
        }

        DirectoryEntry result;
        result.name = name;
        result.path = std::move(child);
        result.directory = S_ISDIR(st.st_mode);
        result.size = result.directory ? 0 : static_cast<std::uint64_t>(st.st_size);
        entries.push_back(std::move(result));
    }
    closedir(dir);

    std::sort(entries.begin(), entries.end(), [](const DirectoryEntry& left, const DirectoryEntry& right) {
        if (left.directory != right.directory) {
            return left.directory > right.directory;
        }
        return left.name < right.name;
    });
    return entries;
}

bool read_file_text(const std::string& normalized_path, std::string& out)
{
    const std::string full_path = full_sd_path(normalized_path);
    struct stat st {};
    if (stat(full_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode) || st.st_size > kMaxEditableFileBytes) {
        return false;
    }

    FILE* file = std::fopen(full_path.c_str(), "rb");
    if (!file) {
        return false;
    }

    out.clear();
    out.reserve(static_cast<std::size_t>(st.st_size));
    std::array<char, kScratchBytes> buffer {};
    while (true) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
            out.append(buffer.data(), read);
        }
        if (read < buffer.size()) {
            const bool failed = std::ferror(file) != 0;
            std::fclose(file);
            if (!failed && out.find('\0') != std::string::npos) {
                return false;
            }
            return !failed;
        }
    }
}

esp_err_t write_content_to_file(httpd_req_t* request,
                                const std::string& normalized_path,
                                const std::string& content)
{
    if (content.size() > kMaxEditableFileBytes) {
        httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "File is too large");
        return ESP_FAIL;
    }

    const std::string full_path = full_sd_path(normalized_path);
    const std::string temp_path = make_root_temp_path("save");
    if (temp_path.empty()) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reserve save path");
        return ESP_FAIL;
    }

    FILE* file = std::fopen(temp_path.c_str(), "wb");
    if (!file) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file for writing");
        return ESP_FAIL;
    }

    bool ok = true;
    if (!content.empty()) {
        ok = std::fwrite(content.data(), 1, content.size(), file) == content.size();
    }
    if (!close_written_file(file)) {
        ok = false;
    }
    if (!ok) {
        std::remove(temp_path.c_str());
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file");
        return ESP_FAIL;
    }
    return replace_file_with_temp(request, temp_path, full_path);
}

esp_err_t stream_file(httpd_req_t* request, const std::string& normalized_path)
{
    const std::string full_path = full_sd_path(normalized_path);
    struct stat st {};
    if (stat(full_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    FILE* file = std::fopen(full_path.c_str(), "rb");
    if (!file) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "application/octet-stream");
    std::array<char, kScratchBytes> buffer {};
    while (true) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
            if (httpd_resp_send_chunk(request, buffer.data(), read) != ESP_OK) {
                std::fclose(file);
                return ESP_FAIL;
            }
        }
        if (read < buffer.size()) {
            const bool failed = std::ferror(file) != 0;
            std::fclose(file);
            if (failed) {
                httpd_resp_send_chunk(request, nullptr, 0);
                return ESP_FAIL;
            }
            return httpd_resp_send_chunk(request, nullptr, 0);
        }
    }
}

void append_directory_panel(httpd_req_t* request, const std::string& current_path, const ServerContext& context)
{
    append_chunk(request, "<aside><p class=\"crumbs\">");
    append_chunk(request, html_escape(current_path));
    append_chunk(request, "</p><div class=\"stack\">");
    if (current_path != kRootPath) {
        append_chunk(request,
                     "<a class=\"button\" href=\"");
        append_chunk(request, make_file_href(parent_path(current_path), context));
        append_chunk(request, "\">上一级</a>");
    }

    append_chunk(request,
                 "<form method=\"post\" action=\"");
    append_chunk(request, with_access_token("/mkdir", context));
    append_chunk(request, "\"><input type=\"hidden\" name=\"path\" value=\"");
    append_chunk(request, html_escape(current_path));
    append_chunk(request,
                 "\"><div class=\"row\"><input name=\"name\" placeholder=\"新文件夹名称\" maxlength=\"96\">"
                 "<button type=\"submit\">新建文件夹</button></div></form>");

    append_chunk(request,
                 "<form method=\"post\" action=\"");
    append_chunk(request, with_access_token("/create", context));
    append_chunk(request, "\"><input type=\"hidden\" name=\"path\" value=\"");
    append_chunk(request, html_escape(current_path));
    append_chunk(request,
                 "\"><div class=\"row\"><input name=\"name\" placeholder=\"新文件名称\" maxlength=\"96\">"
                 "<button type=\"submit\">新建文件</button></div></form>");
    append_chunk(request, "</div></aside>");
}

void append_directory_listing(httpd_req_t* request, const std::string& current_path, const ServerContext& context)
{
    const auto entries = read_directory(current_path);
    append_chunk(request,
                 "<section><table><thead><tr><th>名称</th><th>大小</th><th>操作</th></tr></thead><tbody>");
    for (const auto& entry : entries) {
        append_chunk(request, "<tr><td>");
        if (entry.directory) {
            append_chunk(request, "<a href=\"");
            append_chunk(request, make_file_href(entry.path, context));
            append_chunk(request, "\">");
            append_chunk(request, html_escape(entry.name));
            append_chunk(request, "/</a>");
        } else {
            append_chunk(request, html_escape(entry.name));
        }
        append_chunk(request, "</td><td>");
        append_chunk(request, entry.directory ? std::string("-") : format_size(entry.size));
        append_chunk(request, "</td><td><div class=\"actions\">");
        if (!entry.directory) {
            append_chunk(request, "<a class=\"button\" href=\"");
            append_chunk(request, make_editor_href(entry.path, context));
            append_chunk(request, "\">查看/编辑</a><a class=\"button\" href=\"");
            append_chunk(request, make_file_href(entry.path, context));
            append_chunk(request, "\">下载</a>");
        }
        append_chunk(request, "<form method=\"post\" action=\"");
        append_chunk(request, with_access_token("/delete", context));
        append_chunk(request, "\"><input type=\"hidden\" name=\"path\" value=\"");
        append_chunk(request, html_escape(entry.path));
        append_chunk(request,
                     "\"><button class=\"danger\" type=\"submit\" onclick=\"return confirm('确认删除?')\">删除</button></form>");
        append_chunk(request, "</div></td></tr>");
    }
    append_chunk(request, "</tbody></table>");
    if (entries.empty()) {
        append_chunk(request, "<p class=\"notice\">当前目录为空。</p>");
    }
    append_chunk(request, "</section>");
}

esp_err_t show_editor(httpd_req_t* request, ServerContext& context)
{
    if (!authorize_or_send_error(request, context)) {
        return ESP_FAIL;
    }
    const auto path = path_from_query(request, "path");
    if (!path.ok()) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, path_error_text(path.error));
        return ESP_FAIL;
    }
    auto session = begin_sd_or_send_error(request, context);
    if (!session) {
        return ESP_FAIL;
    }

    std::string content;
    if (!read_file_text(path.relative_path, content)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "File cannot be edited or is too large");
        return ESP_FAIL;
    }

    append_header(request, basename(path.relative_path));
    append_directory_panel(request, parent_path(path.relative_path), context);
    append_chunk(request, "<section><form class=\"stack\" method=\"post\" action=\"");
    append_chunk(request, with_access_token("/save", context));
    append_chunk(request, "\"><input type=\"hidden\" name=\"path\" value=\"");
    append_chunk(request, html_escape(path.relative_path));
    append_chunk(request, "\"><label class=\"muted\">");
    append_chunk(request, html_escape(path.relative_path));
    append_chunk(request, "</label><textarea name=\"content\" spellcheck=\"false\">");
    append_chunk(request, html_escape(content));
    append_chunk(request,
                 "</textarea><div class=\"actions\"><button class=\"primary\" type=\"submit\">保存</button>"
                 "<a class=\"button\" href=\"");
    append_chunk(request, make_file_href(parent_path(path.relative_path), context));
    append_chunk(request, "\">返回目录</a></div></form></section>");
    append_footer(request);
    return ESP_OK;
}

std::optional<std::string> read_form_body(httpd_req_t* request, BodyReadError& error)
{
    error = BodyReadError::None;
    if (request->content_len > kMaxFormBodyBytes) {
        error = BodyReadError::TooLarge;
        return std::nullopt;
    }
    std::string body;
    body.resize(request->content_len);
    std::size_t remaining = request->content_len;
    std::size_t offset = 0;
    unsigned receive_timeouts = 0;
    while (remaining > 0) {
        const int received = httpd_req_recv(request, body.data() + offset, remaining);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++receive_timeouts > kMaxReceiveTimeouts) {
                    error = BodyReadError::Timeout;
                    return std::nullopt;
                }
                continue;
            }
            error = BodyReadError::ReceiveFailed;
            return std::nullopt;
        }
        receive_timeouts = 0;
        offset += static_cast<std::size_t>(received);
        remaining -= static_cast<std::size_t>(received);
    }
    return body;
}

std::optional<std::string> read_raw_body(httpd_req_t* request, std::size_t max_size, BodyReadError& error)
{
    error = BodyReadError::None;
    if (request->content_len > max_size) {
        error = BodyReadError::TooLarge;
        return std::nullopt;
    }

    std::string body;
    body.resize(request->content_len);
    std::size_t remaining = request->content_len;
    std::size_t offset = 0;
    unsigned receive_timeouts = 0;
    while (remaining > 0) {
        const int received = httpd_req_recv(request, body.data() + offset, remaining);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++receive_timeouts > kMaxReceiveTimeouts) {
                    error = BodyReadError::Timeout;
                    return std::nullopt;
                }
                continue;
            }
            error = BodyReadError::ReceiveFailed;
            return std::nullopt;
        }
        receive_timeouts = 0;
        offset += static_cast<std::size_t>(received);
        remaining -= static_cast<std::size_t>(received);
    }
    return body;
}

std::optional<std::string> form_value(const std::string& body, const char* key)
{
    const std::string prefix = std::string(key) + "=";
    std::size_t offset = 0;
    while (offset <= body.size()) {
        const std::size_t next = body.find('&', offset);
        const std::string_view token(body.data() + offset,
                                     (next == std::string::npos ? body.size() : next) - offset);
        if (token.rfind(prefix, 0) == 0) {
            return url_decode(token.substr(prefix.size()));
        }
        if (next == std::string::npos) {
            break;
        }
        offset = next + 1;
    }
    return std::nullopt;
}

std::optional<std::string> child_path_from_form(const std::string& body)
{
    auto directory = form_value(body, "path");
    auto name = form_value(body, "name");
    if (!directory || !name || name->empty() || name->find('/') != std::string::npos) {
        return std::nullopt;
    }

    const auto parent = normalize_request_path(*directory);
    if (!parent.ok()) {
        return std::nullopt;
    }

    std::string child = parent.relative_path;
    if (child != kRootPath) {
        child += "/";
    }
    child += *name;
    const auto normalized = normalize_request_path(child);
    if (!normalized.ok()) {
        return std::nullopt;
    }
    return normalized.relative_path;
}

esp_err_t create_file_handler(httpd_req_t* request)
{
    auto* context = static_cast<ServerContext*>(request->user_ctx);
    if (!context) {
        return ESP_FAIL;
    }
    if (!authorize_or_send_error(request, *context)) {
        return ESP_FAIL;
    }
    BodyReadError body_error {};
    const auto body = read_form_body(request, body_error);
    if (!body) {
        send_body_read_error(request, body_error);
        return ESP_FAIL;
    }
    const auto path = child_path_from_form(*body);
    if (!path) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }
    auto session = begin_sd_or_send_error(request, *context);
    if (!session) {
        return ESP_FAIL;
    }

    const std::string full_path = full_sd_path(*path);
    struct stat existing {};
    if (stat(full_path.c_str(), &existing) == 0) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Path already exists");
        return ESP_FAIL;
    }

    FILE* file = std::fopen(full_path.c_str(), "wb");
    if (!file) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Failed to create file");
        return ESP_FAIL;
    }
    if (!close_written_file(file)) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to finish creating file");
        return ESP_FAIL;
    }
    send_redirect_to(request, make_editor_href(*path, *context));
    return ESP_OK;
}

esp_err_t mkdir_handler(httpd_req_t* request)
{
    auto* context = static_cast<ServerContext*>(request->user_ctx);
    if (!context) {
        return ESP_FAIL;
    }
    if (!authorize_or_send_error(request, *context)) {
        return ESP_FAIL;
    }
    BodyReadError body_error {};
    const auto body = read_form_body(request, body_error);
    if (!body) {
        send_body_read_error(request, body_error);
        return ESP_FAIL;
    }
    const auto path = child_path_from_form(*body);
    if (!path) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }
    auto session = begin_sd_or_send_error(request, *context);
    if (!session) {
        return ESP_FAIL;
    }

    const std::string full_path = full_sd_path(*path);
    if (mkdir(full_path.c_str(), 0775) != 0) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Failed to create directory");
        return ESP_FAIL;
    }
    send_redirect_to(request, make_file_href(parent_path(*path), *context));
    return ESP_OK;
}

esp_err_t save_handler(httpd_req_t* request)
{
    auto* context = static_cast<ServerContext*>(request->user_ctx);
    if (!context) {
        return ESP_FAIL;
    }
    if (!authorize_or_send_error(request, *context)) {
        return ESP_FAIL;
    }
    BodyReadError body_error {};
    const auto body = read_form_body(request, body_error);
    if (!body) {
        send_body_read_error(request, body_error);
        return ESP_FAIL;
    }
    const auto raw_path = form_value(*body, "path");
    const auto raw_content = form_value(*body, "content");
    if (!raw_path || !raw_content) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing form fields");
        return ESP_FAIL;
    }
    const auto path = normalize_request_path(*raw_path);
    if (!path.ok()) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, path_error_text(path.error));
        return ESP_FAIL;
    }
    if (raw_content->size() > kMaxEditableFileBytes) {
        httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "File is too large");
        return ESP_FAIL;
    }

    auto session = begin_sd_or_send_error(request, *context);
    if (!session) {
        return ESP_FAIL;
    }
    if (write_content_to_file(request, path.relative_path, *raw_content) != ESP_OK) {
        return ESP_FAIL;
    }
    send_redirect_to(request, make_editor_href(path.relative_path, *context));
    return ESP_OK;
}

esp_err_t put_file_handler(httpd_req_t* request)
{
    auto* context = static_cast<ServerContext*>(request->user_ctx);
    if (!context) {
        return ESP_FAIL;
    }
    if (!authorize_or_send_error(request, *context)) {
        return ESP_FAIL;
    }
    const auto path = path_from_query(request, "path");
    if (!path.ok()) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, path_error_text(path.error));
        return ESP_FAIL;
    }
    BodyReadError body_error {};
    const auto content = read_raw_body(request, kMaxEditableFileBytes, body_error);
    if (!content) {
        send_body_read_error(request, body_error);
        return ESP_FAIL;
    }
    auto session = begin_sd_or_send_error(request, *context);
    if (!session) {
        return ESP_FAIL;
    }
    if (write_content_to_file(request, path.relative_path, *content) != ESP_OK) {
        return ESP_FAIL;
    }
    send_text(request, HTTPD_200, "text/plain; charset=utf-8", "OK\n");
    return ESP_OK;
}

esp_err_t delete_handler(httpd_req_t* request)
{
    auto* context = static_cast<ServerContext*>(request->user_ctx);
    if (!context) {
        return ESP_FAIL;
    }
    if (!authorize_or_send_error(request, *context)) {
        return ESP_FAIL;
    }

    PathResult path;
    if (request->method == HTTP_DELETE) {
        path = path_from_query(request, "path");
    } else {
        BodyReadError body_error {};
        const auto body = read_form_body(request, body_error);
        if (!body) {
            send_body_read_error(request, body_error);
            return ESP_FAIL;
        }
        const auto raw_path = form_value(*body, "path");
        path = raw_path ? normalize_request_path(*raw_path) : PathResult {};
    }

    if (!path.ok() || path.relative_path == kRootPath) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    auto session = begin_sd_or_send_error(request, *context);
    if (!session) {
        return ESP_FAIL;
    }
    const std::string full_path = full_sd_path(path.relative_path);
    struct stat st {};
    if (stat(full_path.c_str(), &st) != 0) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Path not found");
        return ESP_FAIL;
    }
    const int result = S_ISDIR(st.st_mode) ? rmdir(full_path.c_str()) : unlink(full_path.c_str());
    if (result != 0) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Failed to delete path");
        return ESP_FAIL;
    }

    if (request->method == HTTP_DELETE) {
        send_text(request, HTTPD_200, "text/plain; charset=utf-8", "OK\n");
    } else {
        send_redirect_to(request, make_file_href(parent_path(path.relative_path), *context));
    }
    return ESP_OK;
}

esp_err_t edit_get_handler(httpd_req_t* request)
{
    auto* context = static_cast<ServerContext*>(request->user_ctx);
    if (!context) {
        return ESP_FAIL;
    }
    return show_editor(request, *context);
}

esp_err_t root_get_handler(httpd_req_t* request)
{
    auto* context = static_cast<ServerContext*>(request->user_ctx);
    if (!context) {
        return ESP_FAIL;
    }
    if (!authorize_or_send_error(request, *context)) {
        return ESP_FAIL;
    }

    const auto path = path_from_uri(request);
    if (!path.ok()) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, path_error_text(path.error));
        return ESP_FAIL;
    }
    auto session = begin_sd_or_send_error(request, *context);
    if (!session) {
        return ESP_FAIL;
    }

    const std::string full_path = full_sd_path(path.relative_path);
    struct stat st {};
    if (stat(full_path.c_str(), &st) != 0) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Path not found");
        return ESP_FAIL;
    }
    if (S_ISDIR(st.st_mode)) {
        append_header(request, "Tab5 SD 文件");
        append_directory_panel(request, path.relative_path, *context);
        append_directory_listing(request, path.relative_path, *context);
        append_footer(request);
        return ESP_OK;
    }
    return stream_file(request, path.relative_path);
}

httpd_method_t to_httpd_method(RouteMethod method)
{
    switch (method) {
    case RouteMethod::Get:
        return HTTP_GET;
    case RouteMethod::Post:
        return HTTP_POST;
    case RouteMethod::Put:
        return HTTP_PUT;
    case RouteMethod::Delete:
        return HTTP_DELETE;
    }
    return HTTP_GET;
}

esp_err_t (*handler_for_route(RouteId id))(httpd_req_t*)
{
    switch (id) {
    case RouteId::Edit:
        return edit_get_handler;
    case RouteId::Create:
        return create_file_handler;
    case RouteId::Mkdir:
        return mkdir_handler;
    case RouteId::Save:
        return save_handler;
    case RouteId::DeleteForm:
    case RouteId::ApiFileDelete:
        return delete_handler;
    case RouteId::ApiFilePut:
        return put_file_handler;
    case RouteId::Files:
        return root_get_handler;
    }
    return root_get_handler;
}

httpd_uri_t make_uri(const InternalRouteSpec& route, void* context)
{
    httpd_uri_t result {};
    result.uri = route.uri;
    result.method = to_httpd_method(route.method);
    result.handler = handler_for_route(route.id);
    result.user_ctx = context;
    return result;
}

#endif

} // namespace

std::optional<std::string> url_decode(std::string_view input, bool plus_as_space)
{
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '+' && plus_as_space) {
            output.push_back(' ');
            continue;
        }
        if (ch == '%') {
            if (i + 2 >= input.size()) {
                return std::nullopt;
            }
            const int high = hex_value(input[i + 1]);
            const int low = hex_value(input[i + 2]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            output.push_back(static_cast<char>((high << 4) | low));
            i += 2;
            continue;
        }
        output.push_back(ch);
    }
    return output;
}

std::string url_encode(std::string_view input)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char ch : input) {
        if (is_unreserved_url_char(ch) || ch == '/') {
            output.push_back(static_cast<char>(ch));
        } else {
            output.push_back('%');
            output.push_back(kHex[(ch >> 4U) & 0x0fU]);
            output.push_back(kHex[ch & 0x0fU]);
        }
    }
    return output;
}

std::string html_escape(std::string_view input)
{
    std::string output;
    output.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
        case '&':
            output += "&amp;";
            break;
        case '<':
            output += "&lt;";
            break;
        case '>':
            output += "&gt;";
            break;
        case '"':
            output += "&quot;";
            break;
        case '\'':
            output += "&#39;";
            break;
        default:
            output.push_back(ch);
            break;
        }
    }
    return output;
}

PathResult normalize_request_path(std::string_view input)
{
    if (input.empty()) {
        return { {}, PathError::Empty };
    }
    if (input.front() != kPathSeparator) {
        return { {}, PathError::NotAbsolute };
    }
    if (input.size() > kMaxPathBytes) {
        return { {}, PathError::TooLong };
    }

    std::vector<std::string> segments;
    std::size_t offset = 1;
    while (offset <= input.size()) {
        const std::size_t next = input.find(kPathSeparator, offset);
        const std::size_t end = next == std::string_view::npos ? input.size() : next;
        const std::string_view segment = input.substr(offset, end - offset);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                return { {}, PathError::Traversal };
            }
            for (unsigned char ch : segment) {
                if (is_invalid_fat_path_char(ch)) {
                    return { {}, PathError::InvalidCharacter };
                }
            }
            segments.emplace_back(segment);
        }
        if (next == std::string_view::npos) {
            break;
        }
        offset = next + 1;
    }

    std::string normalized = kRootPath;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            normalized += "/";
        }
        normalized += segments[i];
    }
    return { normalized, PathError::Empty };
}

std::string parent_path(std::string_view normalized_path)
{
    if (normalized_path.empty() || normalized_path == kRootPath) {
        return kRootPath;
    }
    const auto slash = normalized_path.find_last_of('/');
    if (slash == std::string_view::npos || slash == 0) {
        return kRootPath;
    }
    return std::string(normalized_path.substr(0, slash));
}

std::string basename(std::string_view normalized_path)
{
    if (normalized_path.empty() || normalized_path == kRootPath) {
        return {};
    }
    const auto slash = normalized_path.find_last_of('/');
    if (slash == std::string_view::npos) {
        return std::string(normalized_path);
    }
    return std::string(normalized_path.substr(slash + 1));
}

const char* path_error_text(PathError error)
{
    switch (error) {
    case PathError::Empty:
        return "Path is empty";
    case PathError::TooLong:
        return "Path is too long";
    case PathError::NotAbsolute:
        return "Path must start with /";
    case PathError::Traversal:
        return "Path traversal is not allowed";
    case PathError::InvalidCharacter:
        return "Path contains invalid characters";
    }
    return "Invalid path";
}

std::size_t route_spec_count()
{
    return kRoutes.size();
}

RouteSpec route_spec_at(std::size_t index)
{
    if (index >= kRoutes.size()) {
        return {};
    }
    return { kRoutes[index].uri, kRoutes[index].method };
}

#if !defined(TAB5_HOST_TEST)

SdFileManagerServer::~SdFileManagerServer()
{
    stop();
}

bool SdFileManagerServer::start(std::uint16_t port)
{
    if (running_) {
        return true;
    }

    auto* context = new ServerContext;
    if (!context) {
        return false;
    }
    context->sd_mutex = xSemaphoreCreateMutex();
    if (!context->sd_mutex) {
        delete context;
        return false;
    }
    access_token_ = generate_access_token();
    context->access_token = access_token_;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = static_cast<std::uint16_t>(ESP_HTTPD_DEF_CTRL_PORT + 16);
    config.max_uri_handlers = route_spec_count();
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.global_user_ctx = context;
    config.global_user_ctx_free_fn = [](void* ctx) {
        auto* server_context = static_cast<ServerContext*>(ctx);
        if (!server_context) {
            return;
        }
        if (server_context->sd_mutex) {
            vSemaphoreDelete(server_context->sd_mutex);
        }
        delete server_context;
    };

    httpd_handle_t handle = nullptr;
    const esp_err_t err = httpd_start(&handle, &config);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "failed to start SD file manager on port %u: %s",
                 static_cast<unsigned>(port),
                 esp_err_to_name(err));
        config.global_user_ctx_free_fn(context);
        access_token_.clear();
        return false;
    }

    for (const auto& route : kRoutes) {
        const auto handler = make_uri(route, context);
        const esp_err_t register_err = httpd_register_uri_handler(handle, &handler);
        if (register_err != ESP_OK) {
            ESP_LOGW(kTag, "failed to register SD file manager handler %s: %s",
                     handler.uri,
                     esp_err_to_name(register_err));
            httpd_stop(handle);
            access_token_.clear();
            return false;
        }
    }

    server_ = handle;
    running_ = true;
    port_ = port;
    ESP_LOGI(kTag,
             "SD file manager listening on http://tab5-stock.local:%u/?token=%s",
             static_cast<unsigned>(port_),
             access_token_.c_str());
    notify_status("sd web :" + std::to_string(port_) + " see log for token");
    return true;
}

void SdFileManagerServer::stop()
{
    if (server_) {
        httpd_stop(static_cast<httpd_handle_t>(server_));
    }
    server_ = nullptr;
    running_ = false;
    access_token_.clear();
    notify_status({});
}

void SdFileManagerServer::notify_status(const std::string& status)
{
    if (status_callback_) {
        status_callback_(status);
    }
}

#endif

} // namespace tab5::sd_file_manager
