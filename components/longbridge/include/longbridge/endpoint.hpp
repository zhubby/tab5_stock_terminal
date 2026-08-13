#pragma once

#include <string>

namespace tab5::longbridge {

enum class EndpointRegion {
    Global,
    MainlandChina,
};

struct EndpointSet {
    std::string rest_base_url;
    std::string quote_ws_url;
};

EndpointSet default_endpoints(EndpointRegion region);
std::string to_string(EndpointRegion region);
EndpointRegion endpoint_region_from_string(const std::string& value);

} // namespace tab5::longbridge
