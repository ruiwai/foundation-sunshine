#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <d3d11_4.h>
#include <wrl/client.h>
#include "src/platform/windows/pyrowave/shared_fences.h"

namespace {
  using Microsoft::WRL::ComPtr;

  class PyrowaveFences: public testing::Test {
  protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    void SetUp() override {
      const D3D_FEATURE_LEVEL required = D3D_FEATURE_LEVEL_11_0;
      D3D_FEATURE_LEVEL actual {};
      ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        &required, 1, D3D11_SDK_VERSION, &device, &actual, &context));
      ASSERT_EQ(actual, required);
    }
  };

  void wait_for_fence(ID3D11Fence *fence, std::uint64_t value) {
    std::unique_ptr<void, decltype(&CloseHandle)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), &CloseHandle);
    ASSERT_NE(event.get(), nullptr);
    ASSERT_HRESULT_SUCCEEDED(fence->SetEventOnCompletion(value, event.get()));
    ASSERT_EQ(WaitForSingleObject(event.get(), 2000), WAIT_OBJECT_0);
    ASSERT_EQ(fence->GetCompletedValue(), value);
  }

  TEST_F(PyrowaveFences, WarpDirectionalFencesInitializeAtomically) {
    platf::pyrowave_windows::shared_fences_t fences;
    EXPECT_FALSE(fences.wait_release(1, [] { return false; }));
    ASSERT_EQ(fences.initialize(nullptr), E_POINTER);
    ASSERT_EQ(fences.acquire(), nullptr);
    ASSERT_EQ(fences.release(), nullptr);
    ComPtr<ID3D11Device5> device5;
    ASSERT_HRESULT_SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)));
    ASSERT_HRESULT_SUCCEEDED(fences.initialize(device5.Get()));
    auto *acquire = fences.acquire();
    auto *release = fences.release();
    ASSERT_NE(acquire, nullptr);
    ASSERT_NE(release, nullptr);
    ASSERT_NE(acquire, release);
    ASSERT_EQ(acquire->GetCompletedValue(), 0u);
    ASSERT_EQ(release->GetCompletedValue(), 0u);
    ASSERT_EQ(fences.initialize(device5.Get()), E_UNEXPECTED);
    ASSERT_EQ(fences.acquire(), acquire);
    ASSERT_EQ(fences.release(), release);
  }

  TEST_F(PyrowaveFences, WarpDirectionalSharedPayloadsStayIndependent) {
    // WARP D3D payload isolation only, not Vulkan/target GPU validation.
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ASSERT_HRESULT_SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)));
    ASSERT_HRESULT_SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)));
    platf::pyrowave_windows::shared_fences_t fences;
    ASSERT_HRESULT_SUCCEEDED(fences.initialize(device5.Get()));
    const std::array<ID3D11Fence *, 2> originals {fences.acquire(), fences.release()};
    std::array<ComPtr<ID3D11Fence>, 2> aliases;
    for (std::size_t i = 0; i < originals.size(); ++i) {
      HANDLE raw_handle = nullptr;
      ASSERT_HRESULT_SUCCEEDED(originals[i]->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &raw_handle));
      std::unique_ptr<void, decltype(&CloseHandle)> handle(raw_handle, &CloseHandle);
      ASSERT_NE(handle.get(), nullptr);
      ASSERT_HRESULT_SUCCEEDED(device5->OpenSharedFence(handle.get(), IID_PPV_ARGS(&aliases[i])));
      ASSERT_EQ(aliases[i]->GetCompletedValue(), 0u);
    }
    ASSERT_HRESULT_SUCCEEDED(context4->Signal(fences.acquire(), 5));
    context4->Flush();
    ASSERT_NO_FATAL_FAILURE(wait_for_fence(aliases[0].Get(), 5));
    ASSERT_EQ(aliases[1]->GetCompletedValue(), 0u);
    // D3D stand-in for the release producer only; this is not Vulkan proof.
    ASSERT_HRESULT_SUCCEEDED(context4->Signal(fences.release(), 2));
    context4->Flush();
    ASSERT_TRUE(fences.wait_release(2, [] { return false; }));
    ASSERT_NO_FATAL_FAILURE(wait_for_fence(aliases[1].Get(), 2));
    ASSERT_EQ(aliases[0]->GetCompletedValue(), 5u);
    ASSERT_HRESULT_SUCCEEDED(context4->Signal(fences.acquire(), 6));
    context4->Flush();
    ASSERT_NO_FATAL_FAILURE(wait_for_fence(aliases[0].Get(), 6));
    ASSERT_EQ(aliases[1]->GetCompletedValue(), 2u);
    ASSERT_HRESULT_SUCCEEDED(context4->Signal(fences.release(), 3));
    context4->Flush();
    ASSERT_TRUE(fences.wait_release(3, [] { return false; }));
  }

  TEST_F(PyrowaveFences, WarpReleaseWaitFailurePermanentlyPoisonsOwner) {
    // WARP D3D event lifetime only, not Vulkan or target hardware proof.
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ASSERT_HRESULT_SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)));
    ASSERT_HRESULT_SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)));
    platf::pyrowave_windows::shared_fences_t cancelled;
    ASSERT_HRESULT_SUCCEEDED(cancelled.initialize(device5.Get()));
    EXPECT_FALSE(cancelled.wait_release(1, [] { return true; }));
    ASSERT_HRESULT_SUCCEEDED(context4->Signal(cancelled.release(), 1));
    context4->Flush();
    ASSERT_NO_FATAL_FAILURE(wait_for_fence(cancelled.release(), 1));
    EXPECT_FALSE(cancelled.wait_release(1, [] { return false; }));
    platf::pyrowave_windows::shared_fences_t timed_out;
    ASSERT_HRESULT_SUCCEEDED(timed_out.initialize(device5.Get()));
    EXPECT_FALSE(timed_out.wait_release(1, [] { return false; }, std::chrono::milliseconds(10)));
    // Retain the owner's registered event through late completion and cleanup.
    ASSERT_HRESULT_SUCCEEDED(context4->Signal(timed_out.release(), 1));
    context4->Flush();
    ASSERT_NO_FATAL_FAILURE(wait_for_fence(timed_out.release(), 1));
    EXPECT_FALSE(timed_out.wait_release(1, [] { return false; }));
  }
}
