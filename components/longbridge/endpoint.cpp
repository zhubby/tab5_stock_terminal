#include "longbridge/endpoint.hpp"

#include <algorithm>
#include <cctype>

namespace tab5::longbridge {

EndpointSet default_endpoints(EndpointRegion region)
{
    if (region == EndpointRegion::MainlandChina) {
        return {
            "https://openapi.longbridge.cn",
            "wss://openapi-quote.longbridge.cn",
        };
    }

    return {
        "https://openapi.longbridge.com",
        "wss://openapi-quote.longbridge.com",
    };
}

std::string to_string(EndpointRegion region)
{
    return region == EndpointRegion::MainlandChina ? "cn" : "global";
}

EndpointRegion endpoint_region_from_string(const std::string& value)
{
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (normalized == "cn" || normalized == "china" || normalized == "mainland") {
        return EndpointRegion::MainlandChina;
    }
    return EndpointRegion::Global;
}

} // namespace tab5::longbridge
