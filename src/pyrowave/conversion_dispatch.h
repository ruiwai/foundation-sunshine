/**
 * @file src/pyrowave/conversion_dispatch.h
 * @brief Experimental fused SDR conversion workgroups; no color-math changes.
 */
#pragma once

#include <optional>
#include <string_view>

namespace pyrowave {
  struct conversion_group {
    unsigned x = 8;
    unsigned y = 8;
  };

  constexpr std::optional<conversion_group> parse_conversion_group(std::string_view value) noexcept {
    if (value.empty() || value == "8x8") return conversion_group {8, 8};
    if (value == "16x8") return conversion_group {16, 8};
    if (value == "16x16") return conversion_group {16, 16};
    return std::nullopt;
  }

  // Requires already validated positive even dimensions and an accepted group.
  constexpr conversion_group conversion_dispatch(unsigned width, unsigned height, conversion_group group) noexcept {
    return {(width / 2 + group.x - 1) / group.x, (height / 2 + group.y - 1) / group.y};
  }
}  // namespace pyrowave
