/**
 * @file src/pyrowave/native_fp16.h
 * @brief Host eligibility policy for strict native FP16 encoding.
 */
#pragma once

namespace pyrowave {
  constexpr bool
  use_native_fp16(int bit_depth, int dynamic_hdr_format, bool device_supported, bool precision_overridden) {
    return bit_depth == 8 && dynamic_hdr_format == 0 && device_supported && !precision_overridden;
  }
}  // namespace pyrowave
