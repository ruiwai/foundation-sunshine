#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

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

    void SetUp() override {
      const D3D_FEATURE_LEVEL required = D3D_FEATURE_LEVEL_11_0;
      D3D_FEATURE_LEVEL actual {};
      ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        &required, 1, D3D11_SDK_VERSION, &device, &actual, &context));
      ASSERT_EQ(actual, required);
      ComPtr<ID3DBlob> bytecode, errors;
      const auto path = std::filesystem::path(PYROWAVE_CONVERSION_SHADER_PATH).wstring();
      const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, "main_cs", "cs_5_0",
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
          EXPECT_LE(std::abs(got - expected), code_tolerance) << "plane=" << plane << " x=" << x << " y=" << y;
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

    void run(unsigned w, unsigned h, bool full, unsigned kind, bool borrowed = false) {
      const auto input = pattern(w, h, kind);
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Bytes B,G,R,A; explicitly not sRGB.
      desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA initial {input.data(), w * 4, 0};
      ComPtr<ID3D11Texture2D> source;
      ComPtr<ID3D11Device> producer;
      ComPtr<ID3D11DeviceContext> producer_context;
      ComPtr<ID3D11Texture2D> producer_source;
      ComPtr<IDXGIKeyedMutex> producer_mutex, consumer_mutex;
      std::unique_ptr<void, decltype(&CloseHandle)> shared_handle(nullptr, &CloseHandle);
      if (borrowed) {
        const D3D_FEATURE_LEVEL required = D3D_FEATURE_LEVEL_11_0;
        ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
          &required, 1, D3D11_SDK_VERSION, &producer, nullptr, &producer_context));
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ASSERT_HRESULT_SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, &producer_source));
        ASSERT_HRESULT_SUCCEEDED(producer_source.As(&producer_mutex));
        ASSERT_EQ(producer_mutex->AcquireSync(0, 1000), S_OK);
        producer_context->UpdateSubresource(producer_source.Get(), 0, nullptr, input.data(), w * 4, 0);
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
        producer_context->Flush();
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
      context->CSSetShader(shader.Get(), nullptr, 0);
      context->CSSetShaderResources(0, 1, srv.GetAddressOf());
      context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
      context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
      // Extra group in both axes exercises safe oversized dispatch, even for 64x16.
      if (borrowed) {
        ASSERT_EQ(consumer_mutex->AcquireSync(2, 1000), S_OK);
        ASSERT_EQ(producer_mutex->AcquireSync(0, 0), WAIT_TIMEOUT);
      }
      context->Dispatch((w / 2 + 7) / 8 + 1, (h / 2 + 7) / 8 + 1, 1);
      context->ClearState(); // Unbind SRV, UAVs, constants and shader before copying.
      if (borrowed) {
        context->Flush();
        ASSERT_EQ(consumer_mutex->ReleaseSync(0), S_OK);
        // Reuse the borrowed source before any output Map or CPU completion wait.
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

  TEST_F(PyrowaveConversion, WarpBorrowedSourceHandoffPreservesConvertedPlanes) {
    for (bool full : {false, true}) {
      SCOPED_TRACE(testing::Message() << "full=" << full);
      ASSERT_NO_FATAL_FAILURE(run(18, 10, full, 7, true));
    }
  }

  TEST_F(PyrowaveConversion, WarpMatchesCenteredBt709Reference) {
    // Software WARP correctness only; UHD hardware and performance are deferred.
    for (bool full : {false, true}) {
      for (auto size : {std::array<unsigned, 2>{2, 2}, {18, 10}, {64, 16}}) {
        for (unsigned kind = 0; kind < 8; ++kind) {
          SCOPED_TRACE(testing::Message() << "full=" << full << " size=" << size[0] << 'x' << size[1] << " pattern=" << kind);
          ASSERT_NO_FATAL_FAILURE(run(size[0], size[1], full, kind));
        }
      }
    }
  }
}
