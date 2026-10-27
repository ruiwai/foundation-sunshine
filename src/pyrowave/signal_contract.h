/**
 * @file src/pyrowave/signal_contract.h
 * @brief Restricted SDR signal preflight for explicitly selected PyroWave streams.
 */
#pragma once

#include <string_view>

namespace pyrowave {
  constexpr const char *validate_idle_policy(bool suppress_unchanged) noexcept {
    if (suppress_unchanged)
      return "PyroWave requires variable_refresh_rate disabled until paired-client idle/refresh behavior is verified";
    return nullptr;
  }

  constexpr const char *validate_cursor_policy(bool visible, bool local_mode) noexcept {
    (void) visible;  // Host compositing supports both visible and hidden capture.
    if (local_mode)
      return "PyroWave local-cursor rendering is unsupported; disable local cursor mode";
    return nullptr;
  }

  constexpr const char *validate_capture_backend(bool dxgi, std::string_view backend) noexcept {
    if (!dxgi)
      return "PyroWave requires DXGI capture; select a DXGI hardware device";
    if (!backend.empty() && backend != "ddx" && backend != "vdd")
      return "PyroWave supports Desktop Duplication or direct VDD capture; select auto (empty), ddx or vdd";
    return nullptr;
  }

  // Call only after backend validation; auto uses Desktop Duplication without a VDD probe.
  constexpr bool uses_direct_vdd_capture(std::string_view backend) noexcept {
    return backend == "vdd";
  }

  constexpr const char *validate_capture_target(std::string_view target) noexcept {
    if (!target.empty() && target != "display")
      return "PyroWave supports display capture only; set capture_target=display";
    return nullptr;
  }

  constexpr const char *
  validate_capture_size(int requested_width, int requested_height, int capture_width, int capture_height) noexcept {
    if (requested_width <= 0 || requested_height <= 0 || requested_width % 2 != 0 || requested_height % 2 != 0) {
      return "request positive even width and height for PyroWave YUV420";
    }
    if (capture_width != requested_width || capture_height != requested_height) {
      return "PyroWave requires fixed equal-size capture; set the display to the negotiated size and do not resize or rotate";
    }
    return nullptr;
  }

  constexpr const char *
  validate_sdr_signal(int width, int height, int dynamic_range, int chroma_sampling, int encoder_csc_mode) noexcept {
    if (width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0) {
      return "request positive even width and height for PyroWave YUV420";
    }
    if (dynamic_range != 0) {
      return "select SDR (dynamicRangeMode=0); PyroWave HDR/HLG is not supported";
    }
    if (chroma_sampling != 0) {
      return "select YUV420 (chromaSamplingType=0) for PyroWave";
    }
    if (encoder_csc_mode != 2 && encoder_csc_mode != 3) {
      return "select BT.709 limited/full (encoderCscMode=2 or 3) for PyroWave";
    }
    return nullptr;
  }
}  // namespace pyrowave
