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

namespace platf::pyrowave_windows {
  class shared_fences_t {
  public:
    HRESULT initialize(ID3D11Device5 *device) {
      if (device == nullptr) return E_POINTER;
      if (acquire_ || release_) return E_UNEXPECTED;

      Microsoft::WRL::ComPtr<ID3D11Fence> acquire, release;
      const auto acquire_result = device->CreateFence(
        0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&acquire));
      if (FAILED(acquire_result)) return acquire_result;
      const auto release_result = device->CreateFence(
        0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&release));
      if (FAILED(release_result)) return release_result;

      acquire_.Swap(acquire);
      release_.Swap(release);
      return S_OK;
    }

    ID3D11Fence *acquire() const { return acquire_.Get(); }
    ID3D11Fence *release() const { return release_.Get(); }

  private:
    Microsoft::WRL::ComPtr<ID3D11Fence> acquire_;
    Microsoft::WRL::ComPtr<ID3D11Fence> release_;
  };
}  // namespace platf::pyrowave_windows
