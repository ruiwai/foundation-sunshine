#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "src/platform/windows/display_vram_internal.h"
#include "src/platform/windows/pyrowave/shared_fences.h"
#include "src/pyrowave/conversion_dispatch.h"

namespace {
  using Microsoft::WRL::ComPtr;
  using rgb_t = std::array<double, 3>;
  // Fixed before execution: float shader versus independent double reference.
  constexpr int code_tolerance = 1;

  int reference(rgb_t rgb, unsigned plane, bool full) {
    const double y = (2126.0 * rgb[0] + 7152.0 * rgb[1] + 722.0 * rgb[2]) / 10000.0;
    const double signal = plane == 0 ? y : (plane == 1 ? (rgb[2] - y) / 1.8556 : (rgb[0] - y) / 1.5748);
    const double code = plane == 0 ? (full ? 255.0 * signal : 16.0 + 219.0 * signal) :
                                   128.0 + (full ? 255.0 : 224.0) * signal;
    return static_cast<int>(std::floor(std::clamp(code, 0.0, 255.0) + 0.5));
  }

  std::vector<uint8_t> pattern(unsigned w, unsigned h, unsigned kind) {
    std::vector<uint8_t> bytes(w * h * 4);
    for (unsigned y = 0; y < h; ++y) {
      for (unsigned x = 0; x < w; ++x) {
        std::array<uint8_t, 3> rgb {};
        if (kind == 1) rgb = {255, 255, 255};
        if (kind >= 2 && kind <= 4) rgb[kind - 2] = 255;
        if (kind == 5) rgb.fill(static_cast<uint8_t>((x + y * w) * 255 / (w * h - 1)));
        if (kind == 6) rgb = ((x ^ y) & 1) ? std::array<uint8_t, 3>{255, 255, 255} : std::array<uint8_t, 3>{0, 0, 0};
        // Red left column, blue right column: centered mean != legacy left column.
        if (kind == 7) rgb = (x & 1) ? std::array<uint8_t, 3>{0, 0, 255} : std::array<uint8_t, 3>{255, 0, 0};
        const unsigned i = (y * w + x) * 4;
        bytes[i] = rgb[2]; bytes[i + 1] = rgb[1]; bytes[i + 2] = rgb[0];
        bytes[i + 3] = static_cast<uint8_t>((x * 31 + y * 17) & 255); // Alpha irrelevant.
      }
    }
    return bytes;
  }

  class PyrowaveConversion: public testing::Test {
  protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    pyrowave::conversion_group group;

    void SetUp() override {
      const D3D_FEATURE_LEVEL required = D3D_FEATURE_LEVEL_11_0;
      D3D_FEATURE_LEVEL actual {};
      ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        &required, 1, D3D11_SDK_VERSION, &device, &actual, &context));
      ASSERT_EQ(actual, required);
      ASSERT_NO_FATAL_FAILURE(compile({8, 8}));
    }

    void compile(pyrowave::conversion_group variant) {
      group = variant;
      shader.Reset();
      const auto x = std::to_string(group.x), y = std::to_string(group.y);
      const D3D_SHADER_MACRO macros[] = {
        {"PYROWAVE_GROUP_X", x.c_str()}, {"PYROWAVE_GROUP_Y", y.c_str()}, {nullptr, nullptr}};
      ComPtr<ID3DBlob> bytecode, errors;
      const auto path = std::filesystem::path(PYROWAVE_CONVERSION_SHADER_PATH).wstring();
      const HRESULT hr = D3DCompileFromFile(path.c_str(), macros, nullptr, "main_cs", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
      ASSERT_HRESULT_SUCCEEDED(hr) << (errors ? static_cast<const char *>(errors->GetBufferPointer()) : "No compiler diagnostic");
      ASSERT_HRESULT_SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader));
    }

    void compare(const std::vector<uint8_t> &input, const std::vector<uint8_t> &output,
                 unsigned w, unsigned h, unsigned plane, bool full, unsigned kind) {
      const unsigned scale = plane == 0 ? 1 : 2;
      for (unsigned y = 0; y < h / scale; ++y) {
        for (unsigned x = 0; x < w / scale; ++x) {
          rgb_t mean {};
          for (unsigned dy = 0; dy < scale; ++dy) {
            for (unsigned dx = 0; dx < scale; ++dx) {
              const unsigned i = ((y * scale + dy) * w + x * scale + dx) * 4;
              for (unsigned c = 0; c < 3; ++c) mean[c] += input[i + 2 - c] / (255.0 * scale * scale);
            }
          }
          const int got = output[y * (w / scale) + x];
          const int expected = reference(mean, plane, full);
          EXPECT_LE(std::abs(got - expected), code_tolerance) << "plane=" << plane << " x=" << x << " y=" << y << " got=" << got << " expected=" << expected;
          if (kind <= 1) {
            const int exact = plane != 0 ? 128 : (kind == 0 ? (full ? 0 : 16) : (full ? 255 : 235));
            EXPECT_EQ(got, exact);
          }
          if (kind == 7 && plane != 0) {
            EXPECT_GT(std::abs(expected - reference({1.0, 0.0, 0.0}, plane, full)), 20);
          }
        }
      }
    }

    void run(unsigned w, unsigned h, bool full, unsigned kind, bool borrowed = false, bool rgba = false) {
      const auto input = pattern(w, h, kind);
      auto upload = input; // Keep the independent reference in canonical BGRA order.
      if (rgba) for (size_t i = 0; i < upload.size(); i += 4) std::swap(upload[i], upload[i + 2]);
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
      desc.Format = rgba ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM; // Never sRGB.
      desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA initial {upload.data(), w * 4, 0};
      ComPtr<ID3D11Texture2D> source;
      ComPtr<ID3D11Device> producer;
      ComPtr<ID3D11DeviceContext> producer_context;
      ComPtr<ID3D11Texture2D> producer_source;
      ComPtr<IDXGIKeyedMutex> producer_mutex, consumer_mutex;
      ComPtr<ID3D11Query> producer_completion, consumer_completion;
      std::unique_ptr<void, decltype(&CloseHandle)> shared_handle(nullptr, &CloseHandle);
      if (borrowed) {
        const D3D_FEATURE_LEVEL required = D3D_FEATURE_LEVEL_11_0;
        ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
          &required, 1, D3D11_SDK_VERSION, &producer, nullptr, &producer_context));
        const D3D11_QUERY_DESC event_desc {D3D11_QUERY_EVENT, 0};
        ASSERT_HRESULT_SUCCEEDED(producer->CreateQuery(&event_desc, &producer_completion));
        ASSERT_HRESULT_SUCCEEDED(device->CreateQuery(&event_desc, &consumer_completion));
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ASSERT_HRESULT_SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, &producer_source));
        ASSERT_HRESULT_SUCCEEDED(producer_source.As(&producer_mutex));
        ASSERT_EQ(producer_mutex->AcquireSync(0, 1000), S_OK);
        producer_context->UpdateSubresource(producer_source.Get(), 0, nullptr, upload.data(), w * 4, 0);
        ComPtr<IDXGIResource1> resource;
        ASSERT_HRESULT_SUCCEEDED(producer_source.As(&resource));
        HANDLE handle = nullptr;
        ASSERT_HRESULT_SUCCEEDED(resource->CreateSharedHandle(nullptr,
          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
        shared_handle.reset(handle);
        ComPtr<ID3D11Device1> consumer;
        ASSERT_HRESULT_SUCCEEDED(device.As(&consumer));
        ASSERT_HRESULT_SUCCEEDED(consumer->OpenSharedResource1(shared_handle.get(), IID_PPV_ARGS(&source)));
        ASSERT_HRESULT_SUCCEEDED(source.As(&consumer_mutex));
        ASSERT_EQ(consumer_mutex->AcquireSync(2, 0), WAIT_TIMEOUT);
        ASSERT_TRUE(platf::dxgi::wait_for_gpu_completion(producer_context.Get(), producer_completion.Get(), 1000));
        ASSERT_EQ(producer_mutex->ReleaseSync(2), S_OK);
      }
      else {
        ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, &initial, &source));
      }
      ComPtr<ID3D11ShaderResourceView> srv;
      ASSERT_HRESULT_SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, &srv));
      std::array<ComPtr<ID3D11Texture2D>, 3> planes, staging;
      std::array<ComPtr<ID3D11UnorderedAccessView>, 3> views;
      desc.MiscFlags = 0; // Output planes and staging must not inherit source sharing.
      for (unsigned p = 0; p < 3; ++p) {
        desc.Width = p == 0 ? w : w / 2; desc.Height = p == 0 ? h : h / 2;
        desc.Format = DXGI_FORMAT_R8_UNORM; desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
        ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &planes[p]));
        ASSERT_HRESULT_SUCCEEDED(device->CreateUnorderedAccessView(planes[p].Get(), nullptr, &views[p]));
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging[p]));
      }
      const std::array<UINT, 4> params {full ? 1u : 0u, 0, 0, 0};
      D3D11_BUFFER_DESC cb {};
      cb.ByteWidth = sizeof(params); cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      const D3D11_SUBRESOURCE_DATA cb_data {params.data(), 0, 0};
      ComPtr<ID3D11Buffer> constants;
      ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&cb, &cb_data, &constants));
      ID3D11UnorderedAccessView *uavs[] = {views[0].Get(), views[1].Get(), views[2].Get()};
      // Acquire before binding the source, like production convert_direct.
      if (borrowed) {
        ASSERT_EQ(consumer_mutex->AcquireSync(2, 1000), S_OK);
        ASSERT_EQ(producer_mutex->AcquireSync(0, 0), WAIT_TIMEOUT);
      }
      context->CSSetShader(shader.Get(), nullptr, 0);
      context->CSSetShaderResources(0, 1, srv.GetAddressOf());
      context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
      context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
      // Extra group in both axes exercises safe oversized dispatch, even for 64x16.
      const auto groups = pyrowave::conversion_dispatch(w, h, group);
      context->Dispatch(groups.x + 1, groups.y + 1, 1);
      context->ClearState(); // Unbind SRV, UAVs, constants and shader before copying.
      if (borrowed) {
        ASSERT_TRUE(platf::dxgi::wait_for_gpu_completion(context.Get(), consumer_completion.Get(), 1000));
        ASSERT_EQ(consumer_mutex->ReleaseSync(0), S_OK);
        // Reuse the borrowed source before output Map (after explicit source completion).
        ASSERT_EQ(producer_mutex->AcquireSync(0, 1000), S_OK);
        const auto white = pattern(w, h, 1);
        producer_context->UpdateSubresource(producer_source.Get(), 0, nullptr, white.data(), w * 4, 0);
        producer_context->Flush();
        ASSERT_EQ(producer_mutex->ReleaseSync(2), S_OK);
      }
      for (unsigned p = 0; p < 3; ++p) {
        context->CopyResource(staging[p].Get(), planes[p].Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        ASSERT_HRESULT_SUCCEEDED(context->Map(staging[p].Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const unsigned pw = p == 0 ? w : w / 2, ph = p == 0 ? h : h / 2;
        std::vector<uint8_t> output(pw * ph);
        const auto *data = static_cast<const uint8_t *>(mapped.pData);
        for (unsigned y = 0; y < ph; ++y) std::copy_n(data + y * mapped.RowPitch, pw, output.data() + y * pw);
        context->Unmap(staging[p].Get(), 0);
        compare(input, output, w, h, p, full, kind);
      }
    }
  };

  void wait_for_fence(ID3D11Fence *fence, std::uint64_t value) {
    std::unique_ptr<void, decltype(&CloseHandle)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), &CloseHandle);
    ASSERT_NE(event.get(), nullptr);
    ASSERT_HRESULT_SUCCEEDED(fence->SetEventOnCompletion(value, event.get()));
    ASSERT_EQ(WaitForSingleObject(event.get(), 2000), WAIT_OBJECT_0);
    ASSERT_EQ(fence->GetCompletedValue(), value);
  }

  TEST_F(PyrowaveConversion, WarpDirectionalFencesInitializeAtomically) {
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

  TEST_F(PyrowaveConversion, WarpDirectionalSharedPayloadsStayIndependent) {
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

  TEST_F(PyrowaveConversion, WarpReleaseWaitFailurePermanentlyPoisonsOwner) {
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

  TEST_F(PyrowaveConversion, WarpCompletionQueryHonorsStop) {
    ComPtr<ID3D11Query> query;
    const D3D11_QUERY_DESC desc { D3D11_QUERY_EVENT, 0 };
    ASSERT_EQ(device->CreateQuery(&desc, &query), S_OK);
    int calls = 0;
    EXPECT_FALSE(platf::dxgi::wait_for_gpu_completion(context.Get(), query.Get(), 1000, [&] { ++calls; return true; }));
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(platf::dxgi::wait_for_gpu_completion(context.Get(), query.Get(), 1000));
  }

  TEST_F(PyrowaveConversion, WarpBorrowedSourceHandoffPreservesConvertedPlanes) {
    for (bool full : {false, true}) {
      SCOPED_TRACE(testing::Message() << "full=" << full);
      ASSERT_NO_FATAL_FAILURE(run(18, 10, full, 7, true));
    }
  }

  TEST_F(PyrowaveConversion, WarpMatchesCenteredBt709Reference) {
    // Software WARP correctness only; UHD hardware and performance are deferred.
    for (auto text : {"8x8", "16x8", "16x16"}) {
      SCOPED_TRACE(text);
      ASSERT_NO_FATAL_FAILURE(compile(*pyrowave::parse_conversion_group(text)));
      for (bool rgba : {false, true}) {
        for (bool full : {false, true}) {
          for (auto size : {std::array<unsigned, 2>{2, 2}, {18, 10}, {34, 18}, {64, 16}}) {
            for (unsigned kind = 0; kind < 8; ++kind) {
              SCOPED_TRACE(testing::Message() << "rgba=" << rgba << " full=" << full << " size=" << size[0] << 'x' << size[1] << " pattern=" << kind);
              ASSERT_NO_FATAL_FAILURE(run(size[0], size[1], full, kind, false, rgba));
            }
          }
        }
      }
    }
  }
}
