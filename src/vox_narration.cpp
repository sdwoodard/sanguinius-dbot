#include "sanguinius/vox_narration.hpp"

#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/speech.hpp"
#include "sanguinius/tts.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

// The production AI client permits a request to run for 60 seconds. Keep the
// durable claim beyond that deadline so reconciliation cannot start a second
// model attempt while the first valid request is still returning.
constexpr std::int64_t generation_lease_ms = 65'000;

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::vector<std::string> normalized_words(std::string_view text) {
  std::vector<std::string> words;
  std::string word;
  for (const unsigned char byte : text) {
    if (std::isalnum(byte) != 0) {
      word.push_back(static_cast<char>(std::tolower(byte)));
    } else if (!word.empty()) {
      words.push_back(std::move(word));
      word.clear();
    }
  }
  if (!word.empty())
    words.push_back(std::move(word));
  return words;
}

[[nodiscard]] bool contains_case_insensitive(std::string_view text,
                                             std::string_view needle) {
  const auto lower = [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  };
  return std::ranges::search(text, needle, {}, lower, lower).begin() !=
         text.end();
}

[[nodiscard]] bool contains_non_ascii(const std::string_view text) noexcept {
  return std::ranges::any_of(
      text, [](const unsigned char value) { return value > 0x7FU; });
}

[[nodiscard]] bool contains_identifier(std::string_view text) {
  std::size_t digit_run{};
  for (const unsigned char value : text) {
    if (std::isdigit(value) != 0) {
      ++digit_run;
      if (digit_run >= 12)
        return true;
    } else {
      digit_run = 0;
    }
  }
  for (std::size_t offset = 0; offset + 36 <= text.size(); ++offset) {
    const auto possible = text.substr(offset, 36);
    bool uuid = true;
    for (std::size_t index = 0; index < possible.size(); ++index) {
      const bool hyphen =
          index == 8 || index == 13 || index == 18 || index == 23;
      if ((hyphen && possible[index] != '-') ||
          (!hyphen &&
           std::isxdigit(static_cast<unsigned char>(possible[index])) == 0)) {
        uuid = false;
        break;
      }
    }
    if (uuid)
      return true;
  }
  return false;
}

[[nodiscard]] bool contains_tarot_amount_detail(const std::string_view line) {
  constexpr std::array<std::string_view, 40> number_words{
      "zero",     "one",      "two",      "three",   "four",    "five",
      "six",      "seven",    "eight",    "nine",    "ten",     "eleven",
      "twelve",   "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
      "eighteen", "nineteen", "twenty",   "thirty",  "forty",   "fifty",
      "sixty",    "seventy",  "eighty",   "ninety",  "hundred", "thousand",
      "million",  "billion",  "trillion", "dozen",   "first",   "second",
      "third",    "fourth",   "fifth",    "sixth"};
  constexpr std::array<std::string_view, 10> money_words{
      "coin",    "coins", "credit", "credits", "dollar",
      "dollars", "cent",  "cents",  "cash",    "currency"};
  const auto words = normalized_words(line);
  return std::ranges::any_of(words, [&](const std::string &word) {
    const auto matches = [&word](const std::string_view candidate) {
      return word == candidate;
    };
    return std::ranges::any_of(number_words, matches) ||
           std::ranges::any_of(money_words, matches);
  });
}

struct UnicodeRange {
  std::uint32_t first{};
  std::uint32_t last{};
};

[[nodiscard]] bool decode_utf8_scalar(const std::string_view text,
                                      std::size_t &offset,
                                      std::uint32_t &value) noexcept {
  if (offset >= text.size())
    return false;
  const auto lead = static_cast<unsigned char>(text[offset]);
  std::size_t width{};
  if (lead <= 0x7FU) {
    value = lead;
    width = 1;
  } else if (lead >= 0xC2U && lead <= 0xDFU) {
    value = lead & 0x1FU;
    width = 2;
  } else if (lead >= 0xE0U && lead <= 0xEFU) {
    value = lead & 0x0FU;
    width = 3;
  } else if (lead >= 0xF0U && lead <= 0xF4U) {
    value = lead & 0x07U;
    width = 4;
  } else {
    return false;
  }
  if (offset + width > text.size())
    return false;
  for (std::size_t index = 1; index < width; ++index) {
    const auto continuation = static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xC0U) != 0x80U)
      return false;
    value = (value << 6U) | (continuation & 0x3FU);
  }
  offset += width;
  return true;
}

[[nodiscard]] bool
contains_unicode_decimal_digit(const std::string_view text) noexcept {
  // Unicode 16.0 General_Category=Nd ranges. Generated Tarot flavor must not
  // be able to disguise a numeric Fate detail with a non-ASCII digit set.
  constexpr std::array ranges{
      UnicodeRange{0x0030U, 0x0039U},   UnicodeRange{0x0660U, 0x0669U},
      UnicodeRange{0x06F0U, 0x06F9U},   UnicodeRange{0x07C0U, 0x07C9U},
      UnicodeRange{0x0966U, 0x096FU},   UnicodeRange{0x09E6U, 0x09EFU},
      UnicodeRange{0x0A66U, 0x0A6FU},   UnicodeRange{0x0AE6U, 0x0AEFU},
      UnicodeRange{0x0B66U, 0x0B6FU},   UnicodeRange{0x0BE6U, 0x0BEFU},
      UnicodeRange{0x0C66U, 0x0C6FU},   UnicodeRange{0x0CE6U, 0x0CEFU},
      UnicodeRange{0x0D66U, 0x0D6FU},   UnicodeRange{0x0DE6U, 0x0DEFU},
      UnicodeRange{0x0E50U, 0x0E59U},   UnicodeRange{0x0ED0U, 0x0ED9U},
      UnicodeRange{0x0F20U, 0x0F29U},   UnicodeRange{0x1040U, 0x1049U},
      UnicodeRange{0x1090U, 0x1099U},   UnicodeRange{0x17E0U, 0x17E9U},
      UnicodeRange{0x1810U, 0x1819U},   UnicodeRange{0x1946U, 0x194FU},
      UnicodeRange{0x19D0U, 0x19D9U},   UnicodeRange{0x1A80U, 0x1A89U},
      UnicodeRange{0x1A90U, 0x1A99U},   UnicodeRange{0x1B50U, 0x1B59U},
      UnicodeRange{0x1BB0U, 0x1BB9U},   UnicodeRange{0x1C40U, 0x1C49U},
      UnicodeRange{0x1C50U, 0x1C59U},   UnicodeRange{0xA620U, 0xA629U},
      UnicodeRange{0xA8D0U, 0xA8D9U},   UnicodeRange{0xA900U, 0xA909U},
      UnicodeRange{0xA9D0U, 0xA9D9U},   UnicodeRange{0xA9F0U, 0xA9F9U},
      UnicodeRange{0xAA50U, 0xAA59U},   UnicodeRange{0xABF0U, 0xABF9U},
      UnicodeRange{0xFF10U, 0xFF19U},   UnicodeRange{0x104A0U, 0x104A9U},
      UnicodeRange{0x10D30U, 0x10D39U}, UnicodeRange{0x10D40U, 0x10D49U},
      UnicodeRange{0x11066U, 0x1106FU}, UnicodeRange{0x110F0U, 0x110F9U},
      UnicodeRange{0x11136U, 0x1113FU}, UnicodeRange{0x111D0U, 0x111D9U},
      UnicodeRange{0x112F0U, 0x112F9U}, UnicodeRange{0x11450U, 0x11459U},
      UnicodeRange{0x114D0U, 0x114D9U}, UnicodeRange{0x11650U, 0x11659U},
      UnicodeRange{0x116C0U, 0x116C9U}, UnicodeRange{0x116D0U, 0x116E3U},
      UnicodeRange{0x11730U, 0x11739U}, UnicodeRange{0x118E0U, 0x118E9U},
      UnicodeRange{0x11950U, 0x11959U}, UnicodeRange{0x11BF0U, 0x11BF9U},
      UnicodeRange{0x11C50U, 0x11C59U}, UnicodeRange{0x11D50U, 0x11D59U},
      UnicodeRange{0x11DA0U, 0x11DA9U}, UnicodeRange{0x11F50U, 0x11F59U},
      UnicodeRange{0x16130U, 0x16139U}, UnicodeRange{0x16A60U, 0x16A69U},
      UnicodeRange{0x16AC0U, 0x16AC9U}, UnicodeRange{0x16B50U, 0x16B59U},
      UnicodeRange{0x16D70U, 0x16D79U}, UnicodeRange{0x1CCF0U, 0x1CCF9U},
      UnicodeRange{0x1D7CEU, 0x1D7FFU}, UnicodeRange{0x1E140U, 0x1E149U},
      UnicodeRange{0x1E2F0U, 0x1E2F9U}, UnicodeRange{0x1E4F0U, 0x1E4F9U},
      UnicodeRange{0x1E5F1U, 0x1E5FAU}, UnicodeRange{0x1E950U, 0x1E959U},
      UnicodeRange{0x1FBF0U, 0x1FBF9U}};
  for (std::size_t offset{}; offset < text.size();) {
    std::uint32_t value{};
    if (!decode_utf8_scalar(text, offset, value))
      return true;
    if (std::ranges::any_of(ranges, [value](const UnicodeRange range) {
          return value >= range.first && value <= range.last;
        }))
      return true;
  }
  return false;
}

[[nodiscard]] bool
contains_unicode_numeric_value(const std::string_view text) noexcept {
  // Unicode 16.0 Numeric_Value ranges. This includes decimal digits, vulgar
  // fractions, superscripts, Roman numerals, enclosed numbers, and numeric
  // ideographs so Tarot amount details cannot be disguised for TTS.
  if (contains_unicode_decimal_digit(text))
    return true;
  constexpr std::array ranges{
      UnicodeRange{0x0030U, 0x0039U},   UnicodeRange{0x00B2U, 0x00B3U},
      UnicodeRange{0x00B9U, 0x00B9U},   UnicodeRange{0x00BCU, 0x00BEU},
      UnicodeRange{0x0660U, 0x0669U},   UnicodeRange{0x06F0U, 0x06F9U},
      UnicodeRange{0x07C0U, 0x07C9U},   UnicodeRange{0x0966U, 0x096FU},
      UnicodeRange{0x09E6U, 0x09EFU},   UnicodeRange{0x09F4U, 0x09F9U},
      UnicodeRange{0x0A66U, 0x0A6FU},   UnicodeRange{0x0AE6U, 0x0AEFU},
      UnicodeRange{0x0B66U, 0x0B6FU},   UnicodeRange{0x0B72U, 0x0B77U},
      UnicodeRange{0x0BE6U, 0x0BF2U},   UnicodeRange{0x0C66U, 0x0C6FU},
      UnicodeRange{0x0C78U, 0x0C7EU},   UnicodeRange{0x0CE6U, 0x0CEFU},
      UnicodeRange{0x0D58U, 0x0D5EU},   UnicodeRange{0x0D66U, 0x0D78U},
      UnicodeRange{0x0DE6U, 0x0DEFU},   UnicodeRange{0x0E50U, 0x0E59U},
      UnicodeRange{0x0ED0U, 0x0ED9U},   UnicodeRange{0x0F20U, 0x0F33U},
      UnicodeRange{0x1040U, 0x1049U},   UnicodeRange{0x1090U, 0x1099U},
      UnicodeRange{0x1369U, 0x137CU},   UnicodeRange{0x16EEU, 0x16F0U},
      UnicodeRange{0x17E0U, 0x17E9U},   UnicodeRange{0x17F0U, 0x17F9U},
      UnicodeRange{0x1810U, 0x1819U},   UnicodeRange{0x1946U, 0x194FU},
      UnicodeRange{0x19D0U, 0x19DAU},   UnicodeRange{0x1A80U, 0x1A89U},
      UnicodeRange{0x1A90U, 0x1A99U},   UnicodeRange{0x1B50U, 0x1B59U},
      UnicodeRange{0x1BB0U, 0x1BB9U},   UnicodeRange{0x1C40U, 0x1C49U},
      UnicodeRange{0x1C50U, 0x1C59U},   UnicodeRange{0x2070U, 0x2070U},
      UnicodeRange{0x2074U, 0x2079U},   UnicodeRange{0x2080U, 0x2089U},
      UnicodeRange{0x2150U, 0x2182U},   UnicodeRange{0x2185U, 0x2189U},
      UnicodeRange{0x2460U, 0x249BU},   UnicodeRange{0x24EAU, 0x24FFU},
      UnicodeRange{0x2776U, 0x2793U},   UnicodeRange{0x2CFDU, 0x2CFDU},
      UnicodeRange{0x3007U, 0x3007U},   UnicodeRange{0x3021U, 0x3029U},
      UnicodeRange{0x3038U, 0x303AU},   UnicodeRange{0x3192U, 0x3195U},
      UnicodeRange{0x3220U, 0x3229U},   UnicodeRange{0x3248U, 0x324FU},
      UnicodeRange{0x3251U, 0x325FU},   UnicodeRange{0x3280U, 0x3289U},
      UnicodeRange{0x32B1U, 0x32BFU},   UnicodeRange{0x3405U, 0x3405U},
      UnicodeRange{0x3483U, 0x3483U},   UnicodeRange{0x382AU, 0x382AU},
      UnicodeRange{0x3B4DU, 0x3B4DU},   UnicodeRange{0x4E00U, 0x4E00U},
      UnicodeRange{0x4E03U, 0x4E03U},   UnicodeRange{0x4E07U, 0x4E07U},
      UnicodeRange{0x4E09U, 0x4E09U},   UnicodeRange{0x4E24U, 0x4E24U},
      UnicodeRange{0x4E5DU, 0x4E5DU},   UnicodeRange{0x4E8CU, 0x4E8CU},
      UnicodeRange{0x4E94U, 0x4E94U},   UnicodeRange{0x4E96U, 0x4E96U},
      UnicodeRange{0x4EACU, 0x4EACU},   UnicodeRange{0x4EBFU, 0x4EC0U},
      UnicodeRange{0x4EDFU, 0x4EDFU},   UnicodeRange{0x4EE8U, 0x4EE8U},
      UnicodeRange{0x4F0DU, 0x4F0DU},   UnicodeRange{0x4F70U, 0x4F70U},
      UnicodeRange{0x4FE9U, 0x4FE9U},   UnicodeRange{0x5006U, 0x5006U},
      UnicodeRange{0x5104U, 0x5104U},   UnicodeRange{0x5146U, 0x5146U},
      UnicodeRange{0x5169U, 0x5169U},   UnicodeRange{0x516BU, 0x516BU},
      UnicodeRange{0x516DU, 0x516DU},   UnicodeRange{0x5341U, 0x5341U},
      UnicodeRange{0x5343U, 0x5345U},   UnicodeRange{0x534CU, 0x534CU},
      UnicodeRange{0x53C1U, 0x53C4U},   UnicodeRange{0x56DBU, 0x56DBU},
      UnicodeRange{0x58F1U, 0x58F1U},   UnicodeRange{0x58F9U, 0x58F9U},
      UnicodeRange{0x5E7AU, 0x5E7AU},   UnicodeRange{0x5EFEU, 0x5EFFU},
      UnicodeRange{0x5F0CU, 0x5F0EU},   UnicodeRange{0x5F10U, 0x5F10U},
      UnicodeRange{0x62D0U, 0x62D0U},   UnicodeRange{0x62FEU, 0x62FEU},
      UnicodeRange{0x634CU, 0x634CU},   UnicodeRange{0x67D2U, 0x67D2U},
      UnicodeRange{0x6D1EU, 0x6D1EU},   UnicodeRange{0x6F06U, 0x6F06U},
      UnicodeRange{0x7396U, 0x7396U},   UnicodeRange{0x767EU, 0x767EU},
      UnicodeRange{0x7695U, 0x7695U},   UnicodeRange{0x79EDU, 0x79EDU},
      UnicodeRange{0x8086U, 0x8086U},   UnicodeRange{0x842CU, 0x842CU},
      UnicodeRange{0x8CAEU, 0x8CAEU},   UnicodeRange{0x8CB3U, 0x8CB3U},
      UnicodeRange{0x8D30U, 0x8D30U},   UnicodeRange{0x920EU, 0x920EU},
      UnicodeRange{0x94A9U, 0x94A9U},   UnicodeRange{0x9621U, 0x9621U},
      UnicodeRange{0x9646U, 0x9646U},   UnicodeRange{0x964CU, 0x964CU},
      UnicodeRange{0x9678U, 0x9678U},   UnicodeRange{0x96F6U, 0x96F6U},
      UnicodeRange{0xA620U, 0xA629U},   UnicodeRange{0xA6E6U, 0xA6EFU},
      UnicodeRange{0xA830U, 0xA835U},   UnicodeRange{0xA8D0U, 0xA8D9U},
      UnicodeRange{0xA900U, 0xA909U},   UnicodeRange{0xA9D0U, 0xA9D9U},
      UnicodeRange{0xA9F0U, 0xA9F9U},   UnicodeRange{0xAA50U, 0xAA59U},
      UnicodeRange{0xABF0U, 0xABF9U},   UnicodeRange{0xF96BU, 0xF96BU},
      UnicodeRange{0xF973U, 0xF973U},   UnicodeRange{0xF978U, 0xF978U},
      UnicodeRange{0xF9B2U, 0xF9B2U},   UnicodeRange{0xF9D1U, 0xF9D1U},
      UnicodeRange{0xF9D3U, 0xF9D3U},   UnicodeRange{0xF9FDU, 0xF9FDU},
      UnicodeRange{0xFF10U, 0xFF19U},   UnicodeRange{0x10107U, 0x10133U},
      UnicodeRange{0x10140U, 0x10178U}, UnicodeRange{0x1018AU, 0x1018BU},
      UnicodeRange{0x102E1U, 0x102FBU}, UnicodeRange{0x10320U, 0x10323U},
      UnicodeRange{0x10341U, 0x10341U}, UnicodeRange{0x1034AU, 0x1034AU},
      UnicodeRange{0x103D1U, 0x103D5U}, UnicodeRange{0x104A0U, 0x104A9U},
      UnicodeRange{0x10858U, 0x1085FU}, UnicodeRange{0x10879U, 0x1087FU},
      UnicodeRange{0x108A7U, 0x108AFU}, UnicodeRange{0x108FBU, 0x108FFU},
      UnicodeRange{0x10916U, 0x1091BU}, UnicodeRange{0x109BCU, 0x109BDU},
      UnicodeRange{0x109C0U, 0x109CFU}, UnicodeRange{0x109D2U, 0x109FFU},
      UnicodeRange{0x10A40U, 0x10A48U}, UnicodeRange{0x10A7DU, 0x10A7EU},
      UnicodeRange{0x10A9DU, 0x10A9FU}, UnicodeRange{0x10AEBU, 0x10AEFU},
      UnicodeRange{0x10B58U, 0x10B5FU}, UnicodeRange{0x10B78U, 0x10B7FU},
      UnicodeRange{0x10BA9U, 0x10BAFU}, UnicodeRange{0x10CFAU, 0x10CFFU},
      UnicodeRange{0x10D30U, 0x10D39U}, UnicodeRange{0x10D40U, 0x10D49U},
      UnicodeRange{0x10E60U, 0x10E7EU}, UnicodeRange{0x10F1DU, 0x10F26U},
      UnicodeRange{0x10F51U, 0x10F54U}, UnicodeRange{0x10FC5U, 0x10FCBU},
      UnicodeRange{0x11052U, 0x1106FU}, UnicodeRange{0x110F0U, 0x110F9U},
      UnicodeRange{0x11136U, 0x1113FU}, UnicodeRange{0x111D0U, 0x111D9U},
      UnicodeRange{0x111E1U, 0x111F4U}, UnicodeRange{0x112F0U, 0x112F9U},
      UnicodeRange{0x11450U, 0x11459U}, UnicodeRange{0x114D0U, 0x114D9U},
      UnicodeRange{0x11650U, 0x11659U}, UnicodeRange{0x116C0U, 0x116C9U},
      UnicodeRange{0x116D0U, 0x116E3U}, UnicodeRange{0x11730U, 0x1173BU},
      UnicodeRange{0x118E0U, 0x118F2U}, UnicodeRange{0x11950U, 0x11959U},
      UnicodeRange{0x11BF0U, 0x11BF9U}, UnicodeRange{0x11C50U, 0x11C6CU},
      UnicodeRange{0x11D50U, 0x11D59U}, UnicodeRange{0x11DA0U, 0x11DA9U},
      UnicodeRange{0x11F50U, 0x11F59U}, UnicodeRange{0x11FC0U, 0x11FD4U},
      UnicodeRange{0x12400U, 0x1246EU}, UnicodeRange{0x16130U, 0x16139U},
      UnicodeRange{0x16A60U, 0x16A69U}, UnicodeRange{0x16AC0U, 0x16AC9U},
      UnicodeRange{0x16B50U, 0x16B59U}, UnicodeRange{0x16B5BU, 0x16B61U},
      UnicodeRange{0x16D70U, 0x16D79U}, UnicodeRange{0x16E80U, 0x16E96U},
      UnicodeRange{0x1CCF0U, 0x1CCF9U}, UnicodeRange{0x1D2C0U, 0x1D2D3U},
      UnicodeRange{0x1D2E0U, 0x1D2F3U}, UnicodeRange{0x1D360U, 0x1D378U},
      UnicodeRange{0x1D7CEU, 0x1D7FFU}, UnicodeRange{0x1E140U, 0x1E149U},
      UnicodeRange{0x1E2F0U, 0x1E2F9U}, UnicodeRange{0x1E4F0U, 0x1E4F9U},
      UnicodeRange{0x1E5F1U, 0x1E5FAU}, UnicodeRange{0x1E8C7U, 0x1E8CFU},
      UnicodeRange{0x1E950U, 0x1E959U}, UnicodeRange{0x1EC71U, 0x1ECABU},
      UnicodeRange{0x1ECADU, 0x1ECAFU}, UnicodeRange{0x1ECB1U, 0x1ECB4U},
      UnicodeRange{0x1ED01U, 0x1ED2DU}, UnicodeRange{0x1ED2FU, 0x1ED3DU},
      UnicodeRange{0x1F100U, 0x1F10CU}, UnicodeRange{0x1FBF0U, 0x1FBF9U},
      UnicodeRange{0x20001U, 0x20001U}, UnicodeRange{0x20064U, 0x20064U},
      UnicodeRange{0x200E2U, 0x200E2U}, UnicodeRange{0x20121U, 0x20121U},
      UnicodeRange{0x2092AU, 0x2092AU}, UnicodeRange{0x20983U, 0x20983U},
      UnicodeRange{0x2098CU, 0x2098CU}, UnicodeRange{0x2099CU, 0x2099CU},
      UnicodeRange{0x20AEAU, 0x20AEAU}, UnicodeRange{0x20AFDU, 0x20AFDU},
      UnicodeRange{0x20B19U, 0x20B19U}, UnicodeRange{0x22390U, 0x22390U},
      UnicodeRange{0x22998U, 0x22998U}, UnicodeRange{0x23B1BU, 0x23B1BU},
      UnicodeRange{0x2626DU, 0x2626DU}, UnicodeRange{0x2F890U, 0x2F890U},
  };
  for (std::size_t offset{}; offset < text.size();) {
    std::uint32_t value{};
    if (!decode_utf8_scalar(text, offset, value))
      return true;
    if (std::ranges::any_of(ranges, [value](const UnicodeRange range) {
          return value >= range.first && value <= range.last;
        }))
      return true;
  }
  return false;
}

[[nodiscard]] bool
contains_invisible_unicode(const std::string_view text) noexcept {
  // Reject Unicode format controls and other default-ignorable scalars before
  // privacy-word and appearance-overlap checks. They have no legitimate role
  // in a short TTS line and can split text while remaining inaudible.
  constexpr std::array ranges{
      UnicodeRange{0x00ADU, 0x00ADU},   UnicodeRange{0x0300U, 0x036FU},
      UnicodeRange{0x0600U, 0x0605U},   UnicodeRange{0x061CU, 0x061CU},
      UnicodeRange{0x06DDU, 0x06DDU},   UnicodeRange{0x070FU, 0x070FU},
      UnicodeRange{0x0890U, 0x0891U},   UnicodeRange{0x08E2U, 0x08E2U},
      UnicodeRange{0x115FU, 0x1160U},   UnicodeRange{0x17B4U, 0x17B5U},
      UnicodeRange{0x180BU, 0x180FU},   UnicodeRange{0x1AB0U, 0x1AFFU},
      UnicodeRange{0x1DC0U, 0x1DFFU},   UnicodeRange{0x200BU, 0x200FU},
      UnicodeRange{0x202AU, 0x202EU},   UnicodeRange{0x2060U, 0x206FU},
      UnicodeRange{0x20D0U, 0x20FFU},   UnicodeRange{0x3164U, 0x3164U},
      UnicodeRange{0xFE00U, 0xFE0FU},   UnicodeRange{0xFE20U, 0xFE2FU},
      UnicodeRange{0xFEFFU, 0xFEFFU},   UnicodeRange{0xFFA0U, 0xFFA0U},
      UnicodeRange{0xFFF0U, 0xFFFBU},   UnicodeRange{0x110BDU, 0x110BDU},
      UnicodeRange{0x110CDU, 0x110CDU}, UnicodeRange{0x13430U, 0x1343FU},
      UnicodeRange{0x1BCA0U, 0x1BCA3U}, UnicodeRange{0x1D173U, 0x1D17AU},
      UnicodeRange{0xE0000U, 0xE0FFFU},
  };
  for (std::size_t offset{}; offset < text.size();) {
    std::uint32_t value{};
    if (!decode_utf8_scalar(text, offset, value))
      return true;
    if (std::ranges::any_of(ranges, [value](const UnicodeRange range) {
          return value >= range.first && value <= range.last;
        }))
      return true;
  }
  return false;
}

[[nodiscard]] bool contains_url(std::string_view text) {
  if (contains_case_insensitive(text, "://") ||
      contains_case_insensitive(text, "www."))
    return true;
  constexpr std::array<std::string_view, 15> url_schemes{
      "http", "https", "ftp", "ftps", "file", "data", "mailto", "tel",
      "ws",   "wss",   "irc", "ircs", "ssh",  "sftp", "gopher"};
  while (!text.empty()) {
    const auto separator = text.find_first_of(" \t\r\n");
    auto token = text.substr(0, separator);
    while (!token.empty() && (token.front() == '(' || token.front() == '[' ||
                              token.front() == '{' || token.front() == '\'' ||
                              token.front() == '"'))
      token.remove_prefix(1);
    while (!token.empty() &&
           (token.back() == '.' || token.back() == ',' || token.back() == '!' ||
            token.back() == '?' || token.back() == ';' || token.back() == ':' ||
            token.back() == ')' || token.back() == ']' || token.back() == '}' ||
            token.back() == '\'' || token.back() == '"'))
      token.remove_suffix(1);
    const auto scheme_separator = token.find(':');
    if (scheme_separator != std::string_view::npos && scheme_separator > 0) {
      const auto scheme = token.substr(0, scheme_separator);
      if (std::ranges::any_of(url_schemes, [scheme](const auto known) {
            return contains_case_insensitive(scheme, known) &&
                   scheme.size() == known.size();
          }))
        return true;
    }
    const auto host_end = token.find_first_of("/:?#");
    const auto host = token.substr(0, host_end);
    const auto dot = host.rfind('.');
    if (dot != std::string_view::npos && dot > 0 && dot + 1 < host.size()) {
      const auto suffix = host.substr(dot + 1);
      if (suffix.size() >= 2 && suffix.size() <= 24 &&
          std::ranges::all_of(suffix,
                              [](const unsigned char value) {
                                return std::isalpha(value) != 0;
                              }) &&
          std::ranges::any_of(token.substr(0, dot),
                              [](const unsigned char value) {
                                return std::isalnum(value) != 0;
                              }))
        return true;
      if (std::ranges::count(host, '.') == 3 &&
          std::ranges::all_of(host, [](const unsigned char value) {
            return std::isdigit(value) != 0 || value == '.';
          }))
        return true;
    }
    if (separator == std::string_view::npos)
      break;
    text.remove_prefix(separator + 1);
  }
  return false;
}

[[nodiscard]] bool sensitive_line(std::string_view line,
                                  VoxNarrationFeature feature) {
  constexpr std::array<std::string_view, 30> prohibited{
      "sealed",       "private",   "balance",     "escrow",      "stake",
      "evidence",     "ledger",    "notice",      "memory",      "transcript",
      "relationship", "affinity",  "trust",       "respect",     "familiarity",
      "score",        "reward",    "proposition", "recovery",    "summary",
      "excerpt",      "dimension", "draft",       "points",      "history",
      "fate",         "esteem",    "mirth",       "reliability", "wariness"};
  if (std::ranges::any_of(prohibited,
                          [line](const auto word) {
                            return contains_case_insensitive(line, word);
                          }) ||
      contains_url(line) || line.find('@') != std::string_view::npos ||
      line.find('`') != std::string_view::npos ||
      line.find('#') != std::string_view::npos ||
      line.find('*') != std::string_view::npos ||
      line.find('_') != std::string_view::npos ||
      line.find('~') != std::string_view::npos ||
      line.find('[') != std::string_view::npos ||
      line.find(']') != std::string_view::npos ||
      line.find('<') != std::string_view::npos ||
      line.find('>') != std::string_view::npos ||
      line.find('|') != std::string_view::npos || contains_identifier(line))
    return true;
  if (feature == VoxNarrationFeature::tarot &&
      (contains_unicode_numeric_value(line) ||
       contains_tarot_amount_detail(line)))
    return true;
  return false;
}

enum class LineValidationStatus { accepted, invalid, duplicate };

struct LineValidationResult {
  std::optional<std::string> line;
  LineValidationStatus status{LineValidationStatus::invalid};
};

[[nodiscard]] LineValidationResult
validate_line(std::string line, const VoxNarrationCandidate &candidate) {
  NormalizedTtsText normalized;
  try {
    normalized = normalize_tts_text(line);
  } catch (const std::exception &) {
    return {};
  }
  if (normalized.scalar_count == 0 || normalized.scalar_count > 160 ||
      contains_invisible_unicode(normalized.text) ||
      // The project does not own a complete Unicode normalization and
      // confusable table. Model-authored speech therefore fails closed on
      // non-ASCII text before bytewise privacy vocabulary checks. Public
      // projections remain UTF-8 capable; Chronicle/Tarot use their safe
      // fallback, and appearance generation becomes silence.
      contains_non_ascii(normalized.text) ||
      sensitive_line(normalized.text, candidate.feature))
    return {};
  const auto terminal_punctuation = static_cast<std::size_t>(
      std::ranges::count_if(normalized.text, [](const char value) {
        return value == '.' || value == '!' || value == '?';
      }));
  if (terminal_punctuation > 1)
    return {};
  if (candidate.feature == VoxNarrationFeature::appearance) {
    // An unsafe source cannot be compared reliably and is a validation
    // failure, not evidence that the model copied it.
    if (contains_invisible_unicode(candidate.safe_input) ||
        contains_non_ascii(candidate.safe_input))
      return {};
    if (appearance_narration_too_similar(candidate.safe_input, normalized.text))
      return {.line = std::nullopt, .status = LineValidationStatus::duplicate};
  }
  return {.line = std::move(normalized.text),
          .status = LineValidationStatus::accepted};
}

[[nodiscard]] std::optional<std::string>
validated_line(std::string line, const VoxNarrationCandidate &candidate) {
  return validate_line(std::move(line), candidate).line;
}

[[nodiscard]] LineValidationResult
parse_narration_line(const std::string_view response,
                     const VoxNarrationCandidate &candidate) {
  try {
    const auto parsed = nlohmann::json::parse(response);
    if (!parsed.is_object() || parsed.size() != 1 || !parsed.contains("line") ||
        !parsed.at("line").is_string())
      return {};
    return validate_line(parsed.at("line").get<std::string>(), candidate);
  } catch (const std::exception &) {
    return {};
  }
}

[[nodiscard]] AiRequest
session_flavor_request(const std::string &context,
                       const std::string_view session_id) {
  return {
      .instructions =
          "Prepare one brief entrance and one brief farewell in "
          "Sanguinius's dignified spoken voice. Use only the supplied "
          "public-safe session context. Never infer or mention private "
          "state, money-like amounts, evidence, memories, notices, "
          "transcripts, identifiers, or relationship dimensions. Each "
          "line must be one sentence, at most 160 Unicode scalars, with "
          "no markdown, URL, or mention. Return exact JSON with only "
          "string fields entrance and farewell.",
      .conversation = {{"user", "Public-safe session context:\n" + context}},
      .max_output_tokens = 160,
      .json_schema =
          AiRequest::JsonSchema{
              .name = "vox_session_flavor",
              .schema =
                  R"({"type":"object","properties":{"entrance":{"type":"string","minLength":1,"maxLength":640},"farewell":{"type":"string","minLength":1,"maxLength":640}},"required":["entrance","farewell"],"additionalProperties":false})",
              .strict = true},
      .purpose = AiPurpose::vox_session,
      .priority = AiPriority::optional,
      .requester_user_id = std::nullopt,
      .idempotency_key = "ai:vox-session:" + std::string{session_id},
  };
}

[[nodiscard]] std::optional<std::pair<std::string, std::string>>
parse_session_flavor(const std::string_view response,
                     const std::string &context) {
  try {
    const auto parsed = nlohmann::json::parse(response);
    if (!parsed.is_object() || parsed.size() != 2 ||
        !parsed.contains("entrance") || !parsed.at("entrance").is_string() ||
        !parsed.contains("farewell") || !parsed.at("farewell").is_string())
      return std::nullopt;
    const VoxNarrationCandidate candidate{
        .intent_id = {},
        .revision = 1,
        .source_event_id = {},
        .event_type = "vox.session_connecting.v1",
        .feature = VoxNarrationFeature::session,
        .guild_id = {},
        .channel_id = {},
        .safe_input = context,
        .fallback_line = std::nullopt,
        .rank = 0,
        .created_at_ms = 0,
        .expires_at_ms = 1,
        .session_id = {},
        .counterpart_outbox_id = std::nullopt,
        .counterpart_required = false,
        .is_test = false};
    auto entrance =
        validated_line(parsed.at("entrance").get<std::string>(), candidate);
    auto farewell =
        validated_line(parsed.at("farewell").get<std::string>(), candidate);
    if (!entrance || !farewell)
      return std::nullopt;
    return std::pair{std::move(*entrance), std::move(*farewell)};
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

} // namespace

std::optional<VoxNarrationPolicy>
vox_narration_policy(const std::string_view event_type) {
  if (event_type == "chronicle.title_awarded.v1")
    return VoxNarrationPolicy{VoxNarrationFeature::chronicle,
                              100,
                              120'000,
                              true,
                              false,
                              "A worthy name is entered into the Chronicle."};
  if (event_type == "tarot.wager_resolved.v1" ||
      event_type == "tarot.wager_voided.v1" ||
      event_type == "tarot.house_resolved.v1" ||
      event_type == "tarot.house_voided.v1")
    return VoxNarrationPolicy{
        VoxNarrationFeature::tarot,       90, 120'000, true, false,
        "The Emperor's Tarot has spoken."};
  if (event_type == "chronicle.session_completed.v1")
    return VoxNarrationPolicy{VoxNarrationFeature::chronicle,
                              80,
                              120'000,
                              true,
                              true,
                              "The Chronicle closes for this gathering."};
  if (event_type == "tarot.draw_created.v1")
    return VoxNarrationPolicy{VoxNarrationFeature::tarot,
                              70,
                              120'000,
                              true,
                              false,
                              "The Emperor's Tarot reveals its sign."};
  if (event_type == "chronicle.session_started.v1")
    return VoxNarrationPolicy{
        VoxNarrationFeature::chronicle,           60, 120'000, true, true,
        "The Chronicle opens for this gathering."};
  if (event_type == "tarot.wager_funded.v1" ||
      event_type == "tarot.house_funded.v1")
    return VoxNarrationPolicy{
        VoxNarrationFeature::tarot,  50, 120'000, true, false,
        "The challenge is accepted."};
  if (event_type == "appearance.live_queued.v1")
    return VoxNarrationPolicy{
        VoxNarrationFeature::appearance, 40, 60'000, true, false, std::nullopt};
  return std::nullopt;
}

std::string_view
vox_narration_feature_name(const VoxNarrationFeature feature) noexcept {
  switch (feature) {
  case VoxNarrationFeature::chronicle:
    return "chronicle";
  case VoxNarrationFeature::tarot:
    return "tarot";
  case VoxNarrationFeature::appearance:
    return "appearance";
  case VoxNarrationFeature::session:
    return "session";
  }
  return "chronicle";
}

std::string_view
vox_narration_model_status_name(const VoxNarrationModelStatus status) noexcept {
  switch (status) {
  case VoxNarrationModelStatus::not_requested:
    return "not_requested";
  case VoxNarrationModelStatus::generated:
    return "generated";
  case VoxNarrationModelStatus::fallback:
    return "fallback";
  case VoxNarrationModelStatus::refused:
    return "refused";
  case VoxNarrationModelStatus::failed:
    return "failed";
  case VoxNarrationModelStatus::saturated:
    return "saturated";
  case VoxNarrationModelStatus::duplicate:
    return "duplicate";
  }
  return "failed";
}

std::string
vox_narration_enqueue_response(const VoxNarrationEnqueueResult &result) {
  if (result.status == VoxNarrationEnqueueStatus::accepted)
    return "Eligible test narration event was durably observed; all "
           "freshness, session, quiet, mute, counterpart, budget, and "
           "deduplication gates still apply.";
  if (result.status == VoxNarrationEnqueueStatus::replay)
    return "That test narration event was already observed; no duplicate "
           "intent or speech item was created (" +
           result.reason + ").";
  return "The test narration event was not enqueued (" + result.reason +
         "). No narration gate was bypassed.";
}

AiRequest vox_narration_request(const VoxNarrationCandidate &candidate) {
  const auto feature =
      std::string{vox_narration_feature_name(candidate.feature)};
  return AiRequest{
      .instructions =
          "Write one brief spoken companion sentence in Sanguinius's dignified "
          "voice. Use only the supplied public-safe projection. Never infer or "
          "mention sealed or private information, money-like amounts, "
          "evidence, "
          "memories, notices, transcripts, identifiers, or relationship "
          "scores. "
          "No markdown, URL, mention, or more than 160 Unicode scalars. Return "
          "exact JSON with only a string field named line.",
      .conversation = {{"user", "Feature: " + feature +
                                    "\nPublic-safe projection:\n" +
                                    candidate.safe_input}},
      .max_output_tokens = 96,
      .json_schema =
          AiRequest::JsonSchema{
              .name = "vox_narration_line",
              .schema =
                  R"({"type":"object","properties":{"line":{"type":"string","minLength":1,"maxLength":640}},"required":["line"],"additionalProperties":false})",
              .strict = true},
      .purpose = AiPurpose::vox_narration,
      .priority = AiPriority::optional,
      .requester_user_id = std::nullopt,
      .idempotency_key = "ai:vox-narration:" + candidate.intent_id + ":" +
                         std::to_string(candidate.revision),
  };
}

std::optional<std::string>
parse_vox_narration_line(const std::string_view response,
                         const VoxNarrationCandidate &candidate) {
  return parse_narration_line(response, candidate).line;
}

bool appearance_narration_too_similar(const std::string_view public_text,
                                      const std::string_view narration) {
  if (contains_invisible_unicode(public_text) ||
      contains_invisible_unicode(narration) ||
      contains_non_ascii(public_text) || contains_non_ascii(narration))
    return true;
  const auto public_words = normalized_words(public_text);
  const auto narration_words = normalized_words(narration);
  if (narration_words.empty())
    return true;
  if (public_words.size() >= 8 && narration_words.size() >= 8) {
    for (std::size_t left = 0; left + 8 <= public_words.size(); ++left) {
      for (std::size_t right = 0; right + 8 <= narration_words.size();
           ++right) {
        if (std::equal(
                public_words.begin() + static_cast<std::ptrdiff_t>(left),
                public_words.begin() + static_cast<std::ptrdiff_t>(left + 8),
                narration_words.begin() + static_cast<std::ptrdiff_t>(right)))
          return true;
      }
    }
  }
  std::multiset<std::string> remaining(public_words.begin(),
                                       public_words.end());
  std::size_t shared{};
  for (const auto &word : narration_words) {
    const auto found = remaining.find(word);
    if (found != remaining.end()) {
      ++shared;
      remaining.erase(found);
    }
  }
  return static_cast<double>(shared) /
             static_cast<double>(narration_words.size()) >
         0.60;
}

VoxNarrationService::VoxNarrationService(
    VoxNarrationRepository &repository, const Clock &clock,
    PersistentIdGenerator &ids, Diagnostics &diagnostics, const AiClient &ai,
    AiWorkService &ai_work, std::string instance_id, const bool enabled,
    const bool test_mode, std::function<void()> speech_wakeup,
    SessionFlavorReady session_flavor_ready,
    std::function<void()> orchestration_wakeup)
    : repository_{repository}, clock_{clock}, ids_{ids},
      diagnostics_{diagnostics}, ai_{ai}, ai_work_{ai_work},
      instance_id_{std::move(instance_id)}, enabled_{enabled},
      test_mode_{test_mode}, speech_wakeup_{std::move(speech_wakeup)},
      session_flavor_ready_{std::move(session_flavor_ready)},
      orchestration_wakeup_{std::move(orchestration_wakeup)} {
  if (instance_id_.empty())
    throw std::invalid_argument{"Narration instance ID is required."};
}

VoxNarrationService::~VoxNarrationService() { stop(); }

void VoxNarrationService::start() {
  if (started_ || callbacks_closed_)
    throw std::logic_error{"Narration service may only be started once."};
  started_ = true;
}

void VoxNarrationService::stop() noexcept {
  try {
    static_cast<void>(work_stop_.request_stop());
    started_ = false;
    if (!callbacks_closed_) {
      callbacks_->close_and_wait();
      callbacks_closed_ = true;
    }
    {
      const std::scoped_lock lock{generation_mutex_};
      live_generations_.clear();
    }
  } catch (...) {
  }
}

void VoxNarrationService::prepare_session_flavor(std::string session_id,
                                                 std::string guild_id,
                                                 std::string summoner_user_id) {
  if (!enabled_ || !session_flavor_ready_)
    return;
  const auto correlation = shortened_persistent_id(session_id);
  const auto work_stop_token = work_stop_.get_token();
  const auto submitted = ai_work_.submit_optional(
      [callbacks = callbacks_, this, session_id = std::move(session_id),
       guild_id = std::move(guild_id),
       summoner_user_id = std::move(summoner_user_id),
       work_stop_token](const std::stop_token) mutable {
        static_cast<void>(
            callbacks->invoke([this, session_id = std::move(session_id),
                               guild_id = std::move(guild_id),
                               summoner_user_id = std::move(summoner_user_id),
                               work_stop_token]() mutable {
              try {
                const auto current = now_ms(clock_);
                if (repository_.automatic_speech_admission_suppressed(current))
                  return;
                const auto context = repository_.session_flavor_context(
                    session_id, guild_id, summoner_user_id);
                if (!context)
                  return;
                const auto response =
                    ai_.generate(session_flavor_request(*context, session_id),
                                 work_stop_token);
                auto flavor = parse_session_flavor(response.text, *context);
                const auto current_context = repository_.session_flavor_context(
                    session_id, guild_id, summoner_user_id);
                if (!flavor ||
                    repository_.automatic_speech_admission_suppressed(
                        now_ms(clock_)) ||
                    !current_context || *current_context != *context)
                  return;
                session_flavor_ready_(
                    std::move(session_id), std::move(guild_id),
                    std::move(flavor->first), std::move(flavor->second));
              } catch (const OperationCancelled &) {
              } catch (const std::exception &) {
                diagnostics_.emit({DiagnosticSeverity::warning,
                                   "vox.narration.session_flavor",
                                   "Session-flavor preparation failed safely.",
                                   shortened_persistent_id(session_id)});
              } catch (...) {
              }
            }));
      });
  if (submitted != SubmitResult::accepted) {
    diagnostics_.emit(
        {DiagnosticSeverity::warning, "vox.narration.session_flavor",
         "Session-flavor preparation queue was saturated.", correlation});
  }
}

bool VoxNarrationService::run_one_cycle() {
  constexpr std::size_t pass_limit = 50;
  constexpr std::size_t observation_limit = 32;
  const auto current = now_ms(clock_);
  const auto observed =
      repository_.observe_batch({.now_ms = current,
                                 .enabled = enabled_,
                                 .test_mode = test_mode_,
                                 .limit = observation_limit,
                                 .next_id = [this] { return ids_.next_id(); }});
  auto remaining = pass_limit - std::min(observed, pass_limit);
  const auto reconciled = remaining == 0
                              ? 0
                              : repository_.reconcile(
                                    current, [this] { return ids_.next_id(); },
                                    [this](const std::string_view intent_id) {
                                      return generation_is_live(intent_id);
                                    },
                                    remaining);
  remaining -= std::min(reconciled, remaining);
  const auto backlog = observed >= observation_limit || reconciled != 0 ||
                       (!enabled_ && observed != 0);
  if (!enabled_)
    return backlog;
  if (remaining == 0)
    return true;
  const auto claim_lease_token = ids_.next_id();
  const auto candidate =
      repository_.claim_next({.now_ms = current,
                              .instance_id = instance_id_,
                              .lease_token = claim_lease_token,
                              .transition_id = ids_.next_id(),
                              .lease_until_ms = current + generation_lease_ms,
                              .test_mode = test_mode_});
  if (!candidate)
    return backlog;
  if (candidate->event_type == "chronicle.session_started.v1" ||
      candidate->event_type == "chronicle.session_completed.v1") {
    complete_fallback(*candidate, VoxNarrationModelStatus::not_requested);
    return true;
  }
  {
    const std::scoped_lock lock{generation_mutex_};
    live_generations_.insert(candidate->intent_id);
  }
  const auto work_stop_token = work_stop_.get_token();
  const auto submitted = ai_work_.submit_optional(
      [callbacks = callbacks_, this, candidate = *candidate, claim_lease_token,
       work_stop_token](const std::stop_token) mutable {
        static_cast<void>(
            callbacks->invoke([this, candidate = std::move(candidate),
                               claim_lease_token = std::move(claim_lease_token),
                               work_stop_token]() mutable {
              generate_dispatched(std::move(candidate),
                                  std::move(claim_lease_token),
                                  work_stop_token);
            }));
      });
  if (submitted != SubmitResult::accepted) {
    release_generation(candidate->intent_id);
    complete_fallback(*candidate, VoxNarrationModelStatus::saturated);
  }
  return true;
}

void VoxNarrationService::generate(VoxNarrationCandidate candidate,
                                   const std::stop_token stop_token) noexcept {
  try {
    const auto response =
        ai_.generate(vox_narration_request(candidate), stop_token);
    auto parsed = parse_narration_line(response.text, candidate);
    if (!parsed.line) {
      const auto status = parsed.status == LineValidationStatus::duplicate
                              ? VoxNarrationModelStatus::duplicate
                              : VoxNarrationModelStatus::failed;
      complete_fallback(std::move(candidate), status);
      return;
    }
    const auto bytes =
        std::as_bytes(std::span{parsed.line->data(), parsed.line->size()});
    const auto content_hash = sha256_hex(bytes);
    repository_.complete_generation(
        {.intent_id = candidate.intent_id,
         .expected_revision = candidate.revision,
         .expected_mute_epoch = candidate.mute_epoch,
         .line = std::move(parsed.line),
         .model_status = VoxNarrationModelStatus::generated,
         .content_hash = content_hash,
         .speech_id = ids_.next_id(),
         .transition_id = ids_.next_id(),
         .now_ms = now_ms(clock_)});
    if (speech_wakeup_)
      speech_wakeup_();
    if (orchestration_wakeup_)
      orchestration_wakeup_();
  } catch (const AiRefusal &) {
    complete_fallback(std::move(candidate), VoxNarrationModelStatus::refused);
  } catch (const OperationCancelled &) {
  } catch (const std::exception &) {
    diagnostics_.emit({DiagnosticSeverity::warning, "vox.narration.generate",
                       "Narration generation failed safely.",
                       shortened_persistent_id(candidate.intent_id)});
    complete_fallback(std::move(candidate), VoxNarrationModelStatus::failed);
  } catch (...) {
    complete_fallback(std::move(candidate), VoxNarrationModelStatus::failed);
  }
}

void VoxNarrationService::generate_dispatched(
    VoxNarrationCandidate candidate, std::string claim_lease_token,
    const std::stop_token stop_token) noexcept {
  const auto intent_id = candidate.intent_id;
  try {
    if (!stop_token.stop_requested()) {
      const auto current = now_ms(clock_);
      auto started = repository_.begin_generation(
          {.intent_id = candidate.intent_id,
           .expected_revision = candidate.revision,
           .expected_mute_epoch = candidate.mute_epoch,
           .instance_id = instance_id_,
           .expected_lease_token = std::move(claim_lease_token),
           .lease_token = ids_.next_id(),
           .transition_id = ids_.next_id(),
           .now_ms = current,
           .lease_until_ms = current + generation_lease_ms,
           .test_mode = test_mode_});
      if (started)
        generate(std::move(*started), stop_token);
    }
  } catch (const std::exception &) {
    diagnostics_.emit({DiagnosticSeverity::warning,
                       "vox.narration.generation_start",
                       "Narration generation start failed safely.",
                       shortened_persistent_id(intent_id)});
  } catch (...) {
  }
  release_generation(intent_id);
  if (orchestration_wakeup_)
    orchestration_wakeup_();
}

bool VoxNarrationService::generation_is_live(
    const std::string_view intent_id) const {
  const std::scoped_lock lock{generation_mutex_};
  return live_generations_.contains(std::string{intent_id});
}

void VoxNarrationService::release_generation(
    const std::string_view intent_id) noexcept {
  try {
    const std::scoped_lock lock{generation_mutex_};
    live_generations_.erase(std::string{intent_id});
  } catch (...) {
  }
}

void VoxNarrationService::complete_fallback(
    VoxNarrationCandidate candidate,
    const VoxNarrationModelStatus status) noexcept {
  try {
    std::optional<std::string> line;
    if (candidate.feature != VoxNarrationFeature::appearance)
      line = candidate.fallback_line;
    std::string hash;
    if (line) {
      const auto bytes = std::as_bytes(std::span{line->data(), line->size()});
      hash = sha256_hex(bytes);
    }
    repository_.complete_generation(
        {.intent_id = candidate.intent_id,
         .expected_revision = candidate.revision,
         .expected_mute_epoch = candidate.mute_epoch,
         .line = std::move(line),
         .model_status = line ? VoxNarrationModelStatus::fallback : status,
         .content_hash = std::move(hash),
         .speech_id = ids_.next_id(),
         .transition_id = ids_.next_id(),
         .now_ms = now_ms(clock_)});
    if (speech_wakeup_)
      speech_wakeup_();
    if (orchestration_wakeup_)
      orchestration_wakeup_();
  } catch (const std::exception &) {
    diagnostics_.emit({DiagnosticSeverity::warning, "vox.narration.complete",
                       "Narration completion failed safely.",
                       shortened_persistent_id(candidate.intent_id)});
  } catch (...) {
  }
}

std::optional<VoxNarrationCandidate>
VoxNarrationService::preview(const std::string_view source_event_id) {
  return repository_.preview(source_event_id, now_ms(clock_));
}

VoxNarrationEnqueueResult
VoxNarrationService::enqueue(const std::string_view source_event_id) {
  auto result = repository_.enqueue_reference(
      {.source_event_id = std::string{source_event_id},
       .now_ms = now_ms(clock_),
       .enabled = enabled_,
       .test_mode = test_mode_,
       .next_id = [this] { return ids_.next_id(); }});
  if (result.status != VoxNarrationEnqueueStatus::rejected)
    if (orchestration_wakeup_)
      orchestration_wakeup_();
  return result;
}

std::string VoxNarrationService::enqueue_with_receipt(
    const std::string_view source_event_id,
    const VoxNarrationControlContext &context) {
  auto response = repository_.enqueue_reference_with_receipt(
      {.source_event_id = std::string{source_event_id},
       .now_ms = now_ms(clock_),
       .enabled = enabled_,
       .test_mode = test_mode_,
       .next_id = [this] { return ids_.next_id(); }},
      context);
  if (orchestration_wakeup_)
    orchestration_wakeup_();
  return response;
}

std::vector<VoxNarrationRecent>
VoxNarrationService::recent(const std::size_t limit) {
  return repository_.recent(std::min<std::size_t>(limit, 10));
}

VoxNarrationHealth VoxNarrationService::health() {
  return repository_.health();
}

std::optional<std::string> VoxNarrationService::control_receipt(
    const VoxNarrationControlContext &context) {
  return repository_.control_receipt(context);
}

std::string VoxNarrationService::record_control_receipt(
    const VoxNarrationControlContext &context, std::string message) {
  return repository_.record_control_receipt(context, std::move(message));
}

} // namespace sanguinius
