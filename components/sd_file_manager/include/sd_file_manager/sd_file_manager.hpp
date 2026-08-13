#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tab5::sd_file_manager {

constexpr std::uint16_t kDefaultPort = 8080;
constexpr std::size_t kMaxEditableFileBytes = 256 * 1024;
constexpr std::size_t kMaxPathBytes = 240;

enum class PathError {
    Empty,
    TooLong,
    NotAbsolute,
    Traversal,
    InvalidCharacter,
};

struct PathResult {
    std::string relative_path;
    PathError error { PathError::Empty };

    bool ok() const { return !relative_path.empty(); }
};

enum class RouteMethod {
    Get,
    Post,
    Put,
    Delete,
};

struct RouteSpec {
    const char* uri { nullptr };
    RouteMethod method { RouteMethod::Get };
};

std::optional<std::string> url_decode(std::string_view input, bool plus_as_space = true);
std::string url_encode(std::string_view input);
std::string html_escape(std::string_view input);
PathResult normalize_request_path(std::string_view input);
std::string parent_path(std::string_view normalized_path);
std::string basename(std::string_view normalized_path);
const char* path_error_text(PathError error);
std::size_t route_spec_count();
RouteSpec route_spec_at(std::size_t index);

#if !defined(TAB5_HOST_TEST)

class SdFileManagerServer {
public:
    using StatusCallback = std::function<void(const std::string&)>;

    SdFileManagerServer() = default;
    ~SdFileManagerServer();

    bool start(std::uint16_t port = kDefaultPort);
    void stop();
    bool running() const { return running_; }
    std::uint16_t port() const { return port_; }
    const std::string& access_token() const { return access_token_; }

    void on_status(StatusCallback callback) { status_callback_ = std::move(callback); }

private:
    void notify_status(const std::string& status);

    void* server_ { nullptr };
    bool running_ { false };
    std::uint16_t port_ { kDefaultPort };
    std::string access_token_;
    StatusCallback status_callback_;
};

#endif

} // namespace tab5::sd_file_manager
