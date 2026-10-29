/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/pyrowave/shared_fences.h
 * @brief Independent shared fence payloads for D3D11 acquire and Vulkan release.
 */
#pragma once

#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include "src/pyrowave/bounded_wait.h"

namespace platf::pyrowave_windows {
  class shared_fences_t {
  public:
    HRESULT initialize(ID3D11Device5 *device) {
      if (device == nullptr) return E_POINTER;
      if (release_event_ || acquire_ || release_) return E_UNEXPECTED;

      std::unique_ptr<void, decltype(&CloseHandle)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), &CloseHandle);
      if (!event) return HRESULT_FROM_WIN32(GetLastError());
      Microsoft::WRL::ComPtr<ID3D11Fence> acquire, release;
      const auto acquire_result = device->CreateFence(
        0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&acquire));
      if (FAILED(acquire_result)) return acquire_result;
      const auto release_result = device->CreateFence(
        0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&release));
      if (FAILED(release_result)) return release_result;

      release_event_.swap(event);
      acquire_.Swap(acquire);
      release_.Swap(release);
      return S_OK;
    }

    ID3D11Fence *acquire() const { return acquire_.Get(); }
    ID3D11Fence *release() const { return release_.Get(); }

    template<class Stop>
    bool wait_release(std::uint64_t value, Stop &&stop, std::chrono::steady_clock::duration timeout = std::chrono::seconds(1)) {
      if (wait_failed_ || !release_event_ || !acquire_ || !release_) return false;
      const auto result = pyrowave::bounded_event_wait([&] {
        const auto completed = release_->GetCompletedValue();
        if (completed == UINT64_MAX) return pyrowave::wait_result::failed;
        return completed >= value ? pyrowave::wait_result::complete : pyrowave::wait_result::pending;
      }, [&] {
        return ResetEvent(release_event_.get()) && SUCCEEDED(release_->SetEventOnCompletion(value, release_event_.get()));
      }, [&](std::chrono::milliseconds slice) {
        const auto status = WaitForSingleObject(release_event_.get(), static_cast<DWORD>(slice.count()));
        return status == WAIT_OBJECT_0 || status == WAIT_TIMEOUT;
      }, stop, std::chrono::steady_clock::now, timeout);
      // A failed wait may leave an old registration: never reset/rearm it.
      wait_failed_ = result != pyrowave::wait_result::complete;
      return !wait_failed_;
    }

  private:
    // Declared first: keep registrations alive through fence destruction.
    std::unique_ptr<void, decltype(&CloseHandle)> release_event_ {nullptr, &CloseHandle};
    Microsoft::WRL::ComPtr<ID3D11Fence> acquire_;
    Microsoft::WRL::ComPtr<ID3D11Fence> release_;
    bool wait_failed_ = false;
  };
}  // namespace platf::pyrowave_windows
