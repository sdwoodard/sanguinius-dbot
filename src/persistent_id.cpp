#include "sanguinius/persistent_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace sanguinius {
namespace {

[[nodiscard]] bool lowercase_hex(const char character) noexcept {
  return (character >= '0' && character <= '9') ||
         (character >= 'a' && character <= 'f');
}

} // namespace

std::string UuidV4Generator::next_id() {
  std::random_device random;
  std::array<std::uint8_t, 16> bytes{};
  for (auto &byte : bytes) {
    byte = static_cast<std::uint8_t>(random() & 0xffU);
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

  constexpr std::array<char, 16> hexadecimal{'0', '1', '2', '3', '4', '5',
                                             '6', '7', '8', '9', 'a', 'b',
                                             'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    result.push_back(hexadecimal[bytes[index] >> 4U]);
    result.push_back(hexadecimal[bytes[index] & 0x0fU]);
  }
  return result;
}

bool valid_uuid_v4(const std::string &value) noexcept {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' || value[14] != '4' ||
      (value[19] != '8' && value[19] != '9' && value[19] != 'a' &&
       value[19] != 'b')) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      continue;
    }
    if (!lowercase_hex(value[index])) {
      return false;
    }
  }
  return true;
}

} // namespace sanguinius
