/**
 * @file src/platform/windows/display_vram_internal.cpp
 * @brief See display_vram_internal.h.
 */
#include "display_vram_internal.h"

#include <chrono>
#include <utility>

#include "src/logging.h"

namespace platf::dxgi {
  using namespace std::literals;

  bool
  wait_for_gpu_completion(ID3D11DeviceContext *context, ID3D11Query *query, DWORD timeout_ms) {
    if (!context || !query) return false;
    device_t device;
    context->GetDevice(&device);
    if (!device || device->GetDeviceRemovedReason() != S_OK) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    context->Flush();
    for (;;) {
      BOOL complete = FALSE;
      const auto status = context->GetData(query, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (device->GetDeviceRemovedReason() != S_OK) return false;
      if (status == S_OK) return complete == TRUE;
      if (status != S_FALSE || std::chrono::steady_clock::now() >= deadline) return false;
      Sleep(1);
    }
  }

  bool
  pending_pyrowave_source_t::hold(std::shared_ptr<img_d3d_t> image, IDXGIKeyedMutex *mutex) {
    if (!image || !mutex || pending()) return false;
    mutex->AddRef();
    mutex_.reset(mutex);
    image_ = std::move(image);
    return true;
  }

  bool
  pending_pyrowave_source_t::complete() {
    if (!pending()) return true;
    const bool released = image_->borrowed_vdd_frame ? image_->release_borrowed_vdd_after_convert(mutex_.get()) : mutex_->ReleaseSync(image_->encoder_release_key) == S_OK;
    if (!released) return false;
    image_.reset();
    mutex_.reset();
    return true;
  }

  bool
  pending_pyrowave_sources_t::can_hold(std::size_t slot, const img_d3d_t *image, const IDXGIKeyedMutex *mutex) const noexcept {
    if (slot >= size || !image || !mutex || sources_[slot].pending()) return false;
    for (const auto &source : sources_) {
      if (source.owns(image, mutex)) return false;
    }
    return true;
  }

  bool
  pending_pyrowave_sources_t::hold(std::size_t slot, std::shared_ptr<img_d3d_t> image, IDXGIKeyedMutex *mutex) {
    return can_hold(slot, image.get(), mutex) && sources_[slot].hold(std::move(image), mutex);
  }

  bool
  pending_pyrowave_sources_t::complete(std::size_t slot) {
    return slot < size && sources_[slot].complete();
  }

  bool
  pending_pyrowave_sources_t::pending(std::size_t slot) const noexcept {
    return slot >= size || sources_[slot].pending();
  }

  bool
  is_pyrowave_source_valid(const img_d3d_t &img, const D3D11_TEXTURE2D_DESC &desc,
    std::uint64_t adapter_luid, std::uint64_t generation, UINT width, UINT height) noexcept {
    const auto &frame = img.frame_desc;
    return !img.blank && !img.dummy && img.encoder_texture_handle && !img.linear_gamma &&
           width > 0 && height > 0 && (width & 1u) == 0 && (height & 1u) == 0 &&
           (!adapter_luid || !frame.adapter_luid || frame.adapter_luid == adapter_luid) &&
           (!generation || !frame.source_generation || frame.source_generation == generation) &&
           frame.encoding == pixel_encoding_class_e::unorm8 && frame.domain == frame_domain_e::sdr_rec709 &&
           desc.Width == width && desc.Height == height && desc.SampleDesc.Count == 1 &&
           desc.ArraySize == 1 && desc.MipLevels == 1 &&
           (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
  }

  void
  img_d3d_t::note_borrowed_vdd_frame_returned() {
    auto counter = std::move(borrowed_vdd_inflight_counter);
    if (counter) {
      auto current = counter->load(std::memory_order_relaxed);
      while (current > 0 &&
             !counter->compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {}
    }
  }

  void
  img_d3d_t::mark_borrowed_vdd_consumed() {
    borrowed_vdd_frame = false;
    borrowed_vdd_mutex.reset();
    borrowed_vdd_inflight_counter.reset();
    borrowed_vdd_slot = 0;
    encoder_acquire_key = 0;
    encoder_release_key = 0;
    producer_release_key = 0;
  }

  bool
  img_d3d_t::release_borrowed_vdd_after_convert(IDXGIKeyedMutex *encoder_mutex) {
    if (!borrowed_vdd_frame) {
      return true;
    }
    if (!encoder_mutex) {
      BOOST_LOG(warning) << "[vdd] failed to return borrowed slot "sv
                         << borrowed_vdd_slot << ": missing encoder mutex"sv;
      return false;
    }

    HRESULT status = encoder_mutex->ReleaseSync(producer_release_key);
    if (status != S_OK) {
      BOOST_LOG(warning) << "[vdd] failed to return borrowed slot "sv
                         << borrowed_vdd_slot << " after convert [0x"sv
                         << util::hex(status).to_string_view() << ']';
      return false;
    }

    note_borrowed_vdd_frame_returned();
    mark_borrowed_vdd_consumed();
    return true;
  }

  bool
  img_d3d_t::abandon_borrowed_vdd_frame(bool log_busy, DWORD timeout_ms) {
    if (borrowed_vdd_frame && borrowed_vdd_mutex) {
      HRESULT status = borrowed_vdd_mutex->AcquireSync(encoder_acquire_key, timeout_ms);
      if (status == S_OK) {
        status = borrowed_vdd_mutex->ReleaseSync(producer_release_key);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "[vdd] failed to return borrowed slot "sv
                             << borrowed_vdd_slot << " [0x"sv
                             << util::hex(status).to_string_view() << ']';
          return false;
        }
        note_borrowed_vdd_frame_returned();
      }
      else {
        if (log_busy || status != static_cast<HRESULT>(WAIT_TIMEOUT)) {
          BOOST_LOG(warning) << "[vdd] failed to acquire borrowed slot "sv
                             << borrowed_vdd_slot << " for return [0x"sv
                             << util::hex(status).to_string_view() << ']';
        }
        return false;
      }
    }
    mark_borrowed_vdd_consumed();
    return true;
  }

  img_d3d_t::~img_d3d_t() {
    abandon_borrowed_vdd_frame(true, 16);
    if (encoder_texture_handle) {
      CloseHandle(encoder_texture_handle);
    }
  }

  texture_lock_helper::texture_lock_helper(texture_lock_helper &&other) {
    mutex.reset(other.mutex.release());
    locked = other.locked;
    other.locked = false;
  }

  texture_lock_helper &
  texture_lock_helper::operator=(texture_lock_helper &&other) {
    if (this == &other) {
      return *this;
    }
    if (locked && mutex) {
      mutex->ReleaseSync(0);
    }
    mutex.reset(other.mutex.release());
    locked = other.locked;
    other.locked = false;
    return *this;
  }

  texture_lock_helper::texture_lock_helper(IDXGIKeyedMutex *mutex):
      mutex(mutex) {
    if (this->mutex) {
      this->mutex->AddRef();
    }
  }

  texture_lock_helper::~texture_lock_helper() {
    if (locked && mutex) {
      mutex->ReleaseSync(0);
    }
  }

  bool
  texture_lock_helper::lock() {
    if (locked) {
      return true;
    }
    if (!mutex) {
      BOOST_LOG(error) << "Failed to acquire texture mutex: missing IDXGIKeyedMutex"sv;
      return false;
    }

    const HRESULT status = mutex->AcquireSync(0, INFINITE);
    if (status == S_OK) {
      locked = true;
    }
    else {
      BOOST_LOG(error) << "Failed to acquire texture mutex [0x"sv
                       << util::hex(status).to_string_view() << ']';
    }
    return locked;
  }
}  // namespace platf::dxgi
