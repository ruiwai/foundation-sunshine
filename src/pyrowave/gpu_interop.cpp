/**
 * @file src/pyrowave/gpu_interop.cpp
 * @brief Validation for the platform-neutral GPU frame descriptor.
 */
#include "gpu_interop.h"

namespace pyrowave {

  bool
  is_fence_completion_valid(const std::uint64_t completed, const std::uint64_t required) noexcept {
    return completed != UINT64_MAX && completed >= required;
  }

  namespace {

    [[nodiscard]] bool
    valid_format(const image_format_e format) noexcept {
      switch (format) {
      case image_format_e::bgra8_unorm:
      case image_format_e::rgba8_unorm:
      case image_format_e::rgba16_float:
      case image_format_e::nv12:
      case image_format_e::p010:
      case image_format_e::yuv420p:
      case image_format_e::yuv444p:
        return true;
      }

      return false;
    }

    [[nodiscard]] bool
    requires_even_dimensions(const image_format_e format) noexcept {
      return format == image_format_e::nv12 || format == image_format_e::p010 ||
             format == image_format_e::yuv420p;
    }

  }  // namespace

  bool
  is_private_adapter(const std::uint32_t vendor_id, const std::uint32_t device_id) noexcept {
    return vendor_id == 0x8086 &&
           (device_id == 0x4680 || device_id == 0xA780 || device_id == 0xA788);
  }

  bool
  validate(const gpu_frame_t &frame) noexcept {
    return valid_format(frame.format) && frame.width != 0 && frame.width <= 16384 && frame.height != 0 &&
           frame.height <= 16384 && frame.opaque_image != nullptr &&
           (!requires_even_dimensions(frame.format) ||
            (frame.width % 2 == 0 && frame.height % 2 == 0));
  }

}  // namespace pyrowave
