#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tab5::auth {

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t length);
std::array<std::uint8_t, 32> sha256(const std::string& data);

} // namespace tab5::auth
