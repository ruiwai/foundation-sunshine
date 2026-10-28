/**
 * @file src/platform/windows/display_vram_internal.h
 * @brief Shared implementation types for D3D11 VRAM capture backends.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>

#include "display.h"

namespace platf::dxgi {
  bool wait_for_gpu_completion(ID3D11DeviceContext *context, ID3D11Query *query, DWORD timeout_ms, const std::function<bool()> &stop_requested = {});

  struct shared_yuv_plane_t {
    HANDLE shared_handle = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool ten_bit = false;
  };

  /**
   * Direct SDR R8 Y/U/V planes owned by the two-slot conversion ring.
   *
   * PyroWave accepts same-adapter borrowed VDD, owned cursor copies or owned DDX SDR frames without
   * rotation, scaling or enhancement; plane release remains fence-ordered.
   */
  class shared_yuv_encode_device_t: public platf::encode_device_t {
  public:
    virtual ID3D11Device *d3d_device() noexcept = 0;
    virtual ID3D11DeviceContext *d3d_context() noexcept = 0;
    virtual const std::array<shared_yuv_plane_t, 3> *yuv_planes(std::size_t slot) const noexcept = 0;
    virtual bool select_plane_slot(std::size_t slot) noexcept = 0;
    virtual bool release_source_after_encode(std::size_t slot) = 0;
    // SDR-only: clear invalid HDR statistics and return false.
    virtual bool collect_hdr_luminance_stats() = 0;
  };

  [[nodiscard]] std::unique_ptr<shared_yuv_encode_device_t>
  make_shared_yuv_encode_device(std::shared_ptr<display_base_t> display, const ::video::config_t &config, std::function<bool()> stop_requested);

  /**
   * D3D11-backed image shared by capture backends and hardware encoders.
   *
   * This is intentionally kept out of display.h because it is an
   * implementation detail of the Windows VRAM capture path.
   */
  struct img_d3d_t: public platf::img_t {
    texture2d_t capture_texture;
    render_target_t capture_rt;
    keyed_mutex_t capture_mutex;

    HANDLE encoder_texture_handle = {};
    bool dummy = false;
    bool blank = true;
    std::uint32_t id = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool linear_gamma = false;
    bool pyrowave_cursor_composited = false;  // Fresh owned VDD copy, consumed once.
    bool pyrowave_ddx_frame = false;  // Immutable display-owned identity; DDX may re-emit the image.

    // Borrowed VDD frames use the producer's shared texture directly.
    bool borrowed_vdd_texture = false;
    bool borrowed_vdd_frame = false;
    keyed_mutex_t borrowed_vdd_mutex;
    std::shared_ptr<std::atomic<UINT64>> borrowed_vdd_inflight_counter;
    UINT32 borrowed_vdd_slot = 0;
    UINT64 encoder_acquire_key = 0;
    UINT64 encoder_release_key = 0;
    UINT64 producer_release_key = 0;

    void
    note_borrowed_vdd_frame_returned();

    void
    mark_borrowed_vdd_consumed();

    bool
    release_borrowed_vdd_after_convert(IDXGIKeyedMutex *encoder_mutex);

    // Caller owns the source mutex and has proven GPU completion.
    bool
    release_pyrowave_source_after_convert(IDXGIKeyedMutex *encoder_mutex);

    bool
    abandon_borrowed_vdd_frame(bool log_busy = true, DWORD timeout_ms = 0);

    ~img_d3d_t() override;
  };

  // Single-owner lease; holding it never acquires a mutex or submits GPU work.
  class pending_pyrowave_source_t {
  public:
    // Caller already owns mutex. Reject null inputs or an occupied lease.
    bool hold(std::shared_ptr<img_d3d_t> image, IDXGIKeyedMutex *mutex);
    // Only after the shared release fence proves conversion + encode completion.
    // Empty (plane replay) is a no-op; failed releases retain both references.
    bool complete();
    bool pending() const { return static_cast<bool>(image_); }
    bool owns(const img_d3d_t *image, const IDXGIKeyedMutex *mutex) const noexcept {
      return (image && image_.get() == image) || (mutex && mutex_.get() == mutex);
    }
    // Destruction only drops references: never ReleaseSync on unproven abort.
  private:
    std::shared_ptr<img_d3d_t> image_;
    keyed_mutex_t mutex_;
  };

  // Single-thread only; duplicate checks cover exact objects/pointers, not COM aliases.
  class pending_pyrowave_sources_t {
  public:
    static constexpr std::size_t size = 2;
    pending_pyrowave_sources_t() = default;
    pending_pyrowave_sources_t(const pending_pyrowave_sources_t &) = delete;
    pending_pyrowave_sources_t &operator=(const pending_pyrowave_sources_t &) = delete;
    pending_pyrowave_sources_t(pending_pyrowave_sources_t &&) = delete;
    pending_pyrowave_sources_t &operator=(pending_pyrowave_sources_t &&) = delete;
    bool can_hold(std::size_t slot, const img_d3d_t *image, const IDXGIKeyedMutex *mutex) const noexcept;
    bool hold(std::size_t slot, std::shared_ptr<img_d3d_t> image, IDXGIKeyedMutex *mutex);
    bool complete(std::size_t slot);
    bool pending(std::size_t slot) const noexcept;
  private:
    std::array<pending_pyrowave_source_t, size> sources_;
  };

  bool
  is_pyrowave_source_valid(const img_d3d_t &img, const D3D11_TEXTURE2D_DESC &desc,
    std::uint64_t adapter_luid, std::uint64_t generation, UINT width, UINT height) noexcept;

  /**
   * Scoped key-0 lock used while the capture device writes an image.
   */
  struct texture_lock_helper {
    keyed_mutex_t mutex;
    bool locked = false;

    texture_lock_helper(const texture_lock_helper &) = delete;
    texture_lock_helper &
    operator=(const texture_lock_helper &) = delete;

    texture_lock_helper(texture_lock_helper &&other);

    texture_lock_helper &
    operator=(texture_lock_helper &&other);

    texture_lock_helper(IDXGIKeyedMutex *mutex);

    ~texture_lock_helper();

    bool
    lock(DWORD timeout_ms = INFINITE);
  };
}  // namespace platf::dxgi
