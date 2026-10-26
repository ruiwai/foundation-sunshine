/**
 * @file src/platform/windows/display_vdd_vram.cpp
 * @brief D3D11 VRAM capture path for the ZakoVDD shared-texture backend.
 */
#include "display.h"
#include "display_cursor.h"
#include "display_vram_internal.h"
#include "misc.h"

#include <chrono>
#include <utility>

#include "src/config.h"
#include "src/logging.h"

namespace platf::dxgi {
  using namespace std::literals;

  namespace {
    constexpr UINT64 vdd_borrowed_encoder_key = 2;
    constexpr UINT64 vdd_borrow_max_inflight_frames = 2;
    constexpr auto vdd_borrow_telemetry_interval = std::chrono::seconds(5);
  }  // namespace

  void
  display_vdd_vram_t::log_vdd_borrow_debug_telemetry() {
    if (config::sunshine.min_log_level > debug.default_severity()) {
      return;
    }

    const auto log_now = std::chrono::steady_clock::now();
    if (vdd_borrow_last_telemetry.time_since_epoch().count() != 0 &&
        log_now - vdd_borrow_last_telemetry < vdd_borrow_telemetry_interval) {
      return;
    }
    vdd_borrow_last_telemetry = log_now;

    BOOST_LOG(debug) << "[vdd] borrowed texture stats: attempts="sv << vdd_borrow_attempts
                     << " successes="sv << vdd_borrow_successes
                     << " producer_frame="sv << dup.frame_counter()
                     << " slot="sv << dup.producer_slot_index() << "/"sv << dup.producer_slot_count()
                     << " dirty_rects="sv << dup.last_dirty_rect_count()
                     << " replaced_unread="sv << vdd_last_replaced_unread
                     << " dropped_consumer_held="sv << vdd_last_dropped_consumer_held
                     << " dropped_acquire_failures="sv << vdd_last_dropped_acquire_failures
                     << " consumer_acquire_timeouts="sv << dup.consumer_acquire_timeouts()
                     << " deferred="sv << vdd_borrow_deferred_images.size()
                     << " deferred_frames="sv << vdd_borrow_deferred_frames
                     << " returned_deferred="sv << vdd_borrow_returned_deferred_frames
                     << " inflight="sv << vdd_borrow_inflight_frames->load(std::memory_order_relaxed) << "/"sv << vdd_borrow_max_inflight_frames
                     << " inflight_limit_frames="sv << vdd_borrow_inflight_limit_frames;
  }

  capture_e
  display_vdd_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb,
                               std::shared_ptr<platf::img_t> &img_out,
                               std::chrono::milliseconds timeout,
                               bool cursor_visible) {
    if (current_frame) {
      // Defensive: caller forgot to call release_snapshot(). Drop the stale ref.
      dup.release_frame();
      current_frame->Release();
      current_frame = nullptr;
    }

    const bool use_local_cursor = local_cursor_mode_active();
    if (use_local_cursor != vdd_local_cursor_mode_active) {
      // poll_cursor() normally omits an acknowledged shape. Force a refresh
      // whenever ownership moves between the video overlay and the client.
      dup.invalidate_cursor_shape();
      vdd_local_cursor_mode_active = use_local_cursor;
    }

    if (cursor_visible && !use_local_cursor) {
      BOOST_LOG(error) << "[vdd] borrowed-only capture does not support cursor compositing; "sv
                         "use explicit cursor-hidden capture or already-enabled client-local cursor mode"sv;
      return capture_e::error;
    }

    const bool should_poll_cursor = cursor_visible || use_local_cursor;
    std::uint64_t frame_qpc = 0;
    bool cursor_only = false;
    auto status = dup.next_frame(
      timeout,
      &current_frame,
      frame_qpc,
      should_poll_cursor,
      cursor_only
    );
    if (status != capture_e::ok) {
      return status;
    }

    // Ensure the producer keyed-mutex hold and borrowed COM ref are released
    // on every early-return path. release_snapshot() owns them after success.
    bool armed = true;
    auto cleanup = util::fail_guard([&]() {
      if (armed && current_frame) {
        dup.release_frame();
        current_frame->Release();
        current_frame = nullptr;
      }
    });

    vdd_capture_t::cursor_snapshot cursor_state;
    const bool has_cursor_state = should_poll_cursor &&
                                  (cursor_pipeline_ready || use_local_cursor) &&
                                  dup.poll_cursor(cursor_state) &&
                                  cursor_state.valid;
    if (has_cursor_state && use_local_cursor &&
        publish_local_cursor(cursor_state)) {
      dup.acknowledge_cursor_shape(cursor_state.shape_id);
    }

    if (cursor_only) {
      // Local cursor publication above is the only output; do not pull an image.
      return capture_e::timeout;
    }

    const auto now = std::chrono::steady_clock::now();
    auto frame_timestamp = frame_qpc ?
                             now - qpc_time_difference(qpc_counter(), frame_qpc) :
                             now;

    D3D11_TEXTURE2D_DESC desc {};
    current_frame->GetDesc(&desc);
    if (desc.Width != static_cast<UINT>(width_before_rotation) ||
        desc.Height != static_cast<UINT>(height_before_rotation) ||
        desc.Format != capture_format) {
      BOOST_LOG(info) << "[vdd] producer reconfigured: "sv
                      << width_before_rotation << "x"sv << height_before_rotation
                      << " " << dxgi_format_to_string(capture_format)
                      << " -> "sv << desc.Width << "x"sv << desc.Height
                      << " " << dxgi_format_to_string(desc.Format);
      return capture_e::reinit;
    }

    vdd_last_replaced_unread = dup.replaced_unread_frames();
    vdd_last_dropped_consumer_held = dup.dropped_consumer_held_frames();
    vdd_last_dropped_acquire_failures = dup.dropped_acquire_failures();

    auto retire_deferred_borrowed_images = [&]() {
      for (auto it = vdd_borrow_deferred_images.begin();
           it != vdd_borrow_deferred_images.end();) {
        auto deferred = std::static_pointer_cast<img_d3d_t>(*it);
        if (deferred->abandon_borrowed_vdd_frame(false)) {
          ++vdd_borrow_returned_deferred_frames;
          it = vdd_borrow_deferred_images.erase(it);
        }
        else {
          ++it;
        }
      }
    };
    retire_deferred_borrowed_images();

    auto defer_busy_borrowed_image = [&](std::shared_ptr<platf::img_t> busy_img) {
      if (!busy_img) {
        return;
      }
      vdd_borrow_deferred_images.emplace_back(std::move(busy_img));
      ++vdd_borrow_deferred_frames;
    };

    std::shared_ptr<platf::img_t> img;
    std::shared_ptr<img_d3d_t> d3d_img;
    bool pull_interrupted = false;
    auto pull_reusable_image = [&]() -> bool {
      for (int attempt = 0; attempt < 4; ++attempt) {
        if (!pull_free_image_cb(img)) {
          pull_interrupted = true;
          return false;
        }

        d3d_img = std::static_pointer_cast<img_d3d_t>(img);
        if (d3d_img->abandon_borrowed_vdd_frame(false)) {
          d3d_img->blank = false;
          return true;
        }

        BOOST_LOG(debug) << "[vdd] deferring busy borrowed image before reuse"sv;
        defer_busy_borrowed_image(std::move(img));
        d3d_img.reset();
      }
      return false;
    };

    if (!pull_reusable_image()) {
      log_vdd_borrow_debug_telemetry();
      return pull_interrupted ? capture_e::interrupted : capture_e::timeout;
    }

    auto try_borrow_current_frame = [&]() -> capture_e {
      ++vdd_borrow_attempts;
      if (cursor_visible && !use_local_cursor) {
        return capture_e::error;
      }
      if (!vdd_borrow_deferred_images.empty()) {
        return capture_e::timeout;
      }
      if (vdd_borrow_inflight_frames->load(std::memory_order_relaxed) >=
          vdd_borrow_max_inflight_frames) {
        ++vdd_borrow_inflight_limit_frames;
        return capture_e::timeout;
      }

      resource1_t resource;
      auto hr = current_frame->QueryInterface(
        __uuidof(IDXGIResource1),
        reinterpret_cast<void **>(&resource)
      );
      if (FAILED(hr) || !resource) {
        BOOST_LOG(debug) << "[vdd] borrowed texture skipped: IDXGIResource1 unavailable [0x"sv
                         << util::hex(hr).to_string_view() << ']';
        return capture_e::error;
      }

      HANDLE encoder_handle = nullptr;
      hr = resource->CreateSharedHandle(
        nullptr,
        DXGI_SHARED_RESOURCE_READ,
        nullptr,
        &encoder_handle
      );
      if (FAILED(hr) || !encoder_handle) {
        BOOST_LOG(debug) << "[vdd] borrowed texture skipped: CreateSharedHandle failed [0x"sv
                         << util::hex(hr).to_string_view() << ']';
        return capture_e::error;
      }

      if (!d3d_img->abandon_borrowed_vdd_frame(false)) {
        CloseHandle(encoder_handle);
        BOOST_LOG(debug) << "[vdd] borrowed texture skipped: previous borrowed slot is still busy"sv;
        defer_busy_borrowed_image(std::move(img));
        d3d_img.reset();
        return capture_e::timeout;
      }

      IDXGIKeyedMutex *handoff_mutex = nullptr;
      UINT32 handoff_slot = 0;
      const auto handoff_status = dup.handoff_frame(
        vdd_borrowed_encoder_key,
        &handoff_mutex,
        handoff_slot
      );
      if (handoff_status != capture_e::ok || !handoff_mutex) {
        CloseHandle(encoder_handle);
        BOOST_LOG(debug) << "[vdd] borrowed texture skipped: handoff failed"sv;
        return handoff_status == capture_e::ok ? capture_e::error : handoff_status;
      }

      d3d_img->capture_texture.reset(current_frame);
      current_frame = nullptr;
      d3d_img->capture_rt.reset();
      d3d_img->capture_mutex.reset();
      d3d_img->data = nullptr;
      if (d3d_img->encoder_texture_handle) {
        CloseHandle(d3d_img->encoder_texture_handle);
      }
      d3d_img->encoder_texture_handle = encoder_handle;

      d3d_img->pixel_pitch = get_pixel_pitch();
      d3d_img->row_pitch = d3d_img->pixel_pitch * d3d_img->width;
      d3d_img->dummy = false;
      d3d_img->format = desc.Format;
      d3d_img->linear_gamma = capture_linear_gamma;
      d3d_img->borrowed_vdd_texture = true;
      d3d_img->borrowed_vdd_frame = true;
      d3d_img->frame_desc = describe_captured_frame(desc.Format, true);
      d3d_img->borrowed_vdd_mutex.reset(handoff_mutex);
      d3d_img->borrowed_vdd_slot = handoff_slot;
      d3d_img->encoder_acquire_key = vdd_borrowed_encoder_key;
      d3d_img->encoder_release_key = vdd_borrowed_encoder_key;
      d3d_img->producer_release_key = 0;
      d3d_img->data =
        reinterpret_cast<std::uint8_t *>(d3d_img->capture_texture.get());
      d3d_img->borrowed_vdd_inflight_counter = vdd_borrow_inflight_frames;
      ++vdd_borrow_successes;
      vdd_borrow_inflight_frames->fetch_add(1, std::memory_order_relaxed);
      return capture_e::ok;
    };

    status = try_borrow_current_frame();
    log_vdd_borrow_debug_telemetry();
    if (status == capture_e::ok) {
      img_out = img;
      img_out->frame_timestamp = frame_timestamp;
      armed = false;
    }
    return status;
  }

  capture_e
  display_vdd_vram_t::release_snapshot() {
    if (current_frame) {
      current_frame->Release();
      current_frame = nullptr;
    }
    return dup.release_frame();
  }
}  // namespace platf::dxgi
