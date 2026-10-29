#include <gtest/gtest.h>
#include <deque>
#include <type_traits>
#include <vector>

#include "src/platform/windows/display_vram_internal.h"

namespace {
  using namespace platf::dxgi;
  // Scripted API results only, not a simulation of GPU ordering/device loss.
  struct mutex_state {
    std::deque<HRESULT> releases { S_OK };
    std::vector<UINT64> release_keys;
    ULONG refs = 1;
    bool destroyed = false;
  };

  class scripted_mutex final: public IDXGIKeyedMutex {
    mutex_state &state;
  public:
    explicit scripted_mutex(mutex_state &s): state(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
      if (!out) return E_POINTER;
      *out = nullptr;
      if (iid != __uuidof(IUnknown) && iid != __uuidof(IDXGIKeyedMutex)) return E_NOINTERFACE;
      *out = static_cast<IDXGIKeyedMutex *>(this);
      AddRef();
      return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++state.refs; }
    ULONG STDMETHODCALLTYPE Release() override {
      const ULONG remaining = --state.refs;
      if (!remaining) {
        state.destroyed = true;
        delete this;
      }
      return remaining;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AcquireSync(UINT64, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReleaseSync(UINT64 key) override {
      state.release_keys.push_back(key);
      if (state.releases.empty()) return E_FAIL;
      const auto result = state.releases.front();
      state.releases.pop_front();
      return result;
    }
  };

  static_assert(!std::is_copy_constructible_v<pending_pyrowave_source_t>);
  static_assert(!std::is_move_constructible_v<pending_pyrowave_sources_t>);

  TEST(PendingPyrowaveSource, SlotOwnershipDuplicatesAndReplay) {
    mutex_state a, b;
    a.releases = { S_OK, S_OK };
    keyed_mutex_t ma(new scripted_mutex(a)), mb(new scripted_mutex(b));
    auto first = std::make_shared<img_d3d_t>(), second = std::make_shared<img_d3d_t>();
    first->encoder_release_key = 7;
    pending_pyrowave_sources_t leases;
    EXPECT_FALSE(leases.hold(2, first, ma.get()));
    EXPECT_FALSE(leases.hold(0, {}, ma.get()));
    EXPECT_FALSE(leases.hold(0, first, nullptr));
    ASSERT_TRUE(leases.hold(0, first, ma.get()));
    EXPECT_EQ(a.refs, 2u);
    EXPECT_FALSE(leases.hold(0, second, mb.get()));
    EXPECT_FALSE(leases.hold(1, first, mb.get())); // Same image, other mutex.
    EXPECT_FALSE(leases.hold(1, second, ma.get())); // Same pointer, other image.
    ASSERT_TRUE(leases.hold(1, second, mb.get()));
    EXPECT_TRUE(leases.complete(1));
    EXPECT_TRUE(a.release_keys.empty()); // Completing another slot cannot unlock 0.
    EXPECT_TRUE(leases.pending(0));
    EXPECT_FALSE(leases.complete(2));
    EXPECT_TRUE(leases.complete(0));
    EXPECT_TRUE(leases.complete(0)); // Plane replay is a no-op.
    EXPECT_EQ(a.release_keys, (std::vector<UINT64> { 7 }));
    EXPECT_EQ(a.refs, 1u);
    ASSERT_TRUE(leases.hold(1, first, ma.get())); // Ordinary images stay replayable.
    EXPECT_TRUE(leases.complete(1));
    EXPECT_EQ(a.release_keys, (std::vector<UINT64> { 7, 7 }));
  }

  TEST(PendingPyrowaveSource, FailedReleaseRetainsOwnershipUntilRetry) {
    for (HRESULT failure : { E_FAIL, S_FALSE, static_cast<HRESULT>(WAIT_TIMEOUT), DXGI_ERROR_DEVICE_REMOVED }) {
      mutex_state state;
      state.releases = { failure, S_OK };
      keyed_mutex_t mutex(new scripted_mutex(state));
      auto image = std::make_shared<img_d3d_t>();
      std::weak_ptr<img_d3d_t> weak = image;
      pending_pyrowave_sources_t leases;
      ASSERT_TRUE(leases.hold(0, image, mutex.get()));
      image.reset();
      EXPECT_FALSE(leases.complete(0));
      EXPECT_TRUE(leases.pending(0));
      EXPECT_FALSE(weak.expired());
      EXPECT_EQ(state.refs, 2u);
      EXPECT_TRUE(leases.complete(0));
      EXPECT_TRUE(weak.expired());
      EXPECT_EQ(state.refs, 1u);
    }
  }

  TEST(PendingPyrowaveSource, IndependentSessionsCanShareAnOrdinaryImage) {
    mutex_state a, b;
    keyed_mutex_t ma(new scripted_mutex(a)), mb(new scripted_mutex(b));
    auto image = std::make_shared<img_d3d_t>();
    pending_pyrowave_sources_t first_session, second_session;
    ASSERT_TRUE(first_session.hold(0, image, ma.get()));
    ASSERT_TRUE(second_session.hold(1, image, mb.get()));
    EXPECT_TRUE(first_session.complete(0));
    EXPECT_TRUE(second_session.pending(1));
    EXPECT_TRUE(b.release_keys.empty());
    EXPECT_TRUE(second_session.complete(1));
    EXPECT_EQ(a.release_keys.size(), 1u);
    EXPECT_EQ(b.release_keys.size(), 1u);
  }

  TEST(PendingPyrowaveSource, AbortNeverExplicitlyUnlocksUnprovenWork) {
    mutex_state state;
    keyed_mutex_t mutex(new scripted_mutex(state));
    std::weak_ptr<img_d3d_t> weak;
    {
      pending_pyrowave_sources_t leases;
      auto image = std::make_shared<img_d3d_t>();
      weak = image;
      ASSERT_TRUE(leases.hold(0, image, mutex.get()));
      image.reset();
      EXPECT_FALSE(weak.expired());
    }
    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(state.release_keys.empty());
    EXPECT_EQ(state.refs, 1u);
    mutex.reset();
    EXPECT_TRUE(state.destroyed);
  }

  TEST(PendingPyrowaveSource, BorrowedReleaseRetriesBeforeReturningProducerAccounting) {
    mutex_state state;
    state.releases = { S_FALSE, S_OK };
    keyed_mutex_t mutex(new scripted_mutex(state));
    auto image = std::make_shared<img_d3d_t>();
    auto counter = std::make_shared<std::atomic<UINT64>>(1);
    image->borrowed_vdd_texture = image->borrowed_vdd_frame = true;
    image->borrowed_vdd_inflight_counter = counter;
    image->producer_release_key = 11;
    pending_pyrowave_sources_t leases;
    ASSERT_TRUE(leases.hold(1, image, mutex.get()));
    EXPECT_FALSE(leases.complete(1));
    EXPECT_EQ(counter->load(), 1u);
    EXPECT_TRUE(image->borrowed_vdd_frame);
    EXPECT_TRUE(leases.complete(1));
    EXPECT_EQ(counter->load(), 0u);
    EXPECT_FALSE(image->borrowed_vdd_frame);
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> { 11, 11 }));
  }

  TEST(PyrowaveSourceAdmission, OrdinarySharedSourcesAndKnownIdentityChecks) {
    img_d3d_t image;
    // A real owned handle for this pure descriptor test; no production stubs/markers.
    image.encoder_texture_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(image.encoder_texture_handle, nullptr);
    image.blank = false;
    image.frame_desc.domain = platf::frame_domain_e::sdr_rec709;
    image.frame_desc.encoding = platf::pixel_encoding_class_e::unorm8;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 18;
    desc.Height = 10;
    desc.SampleDesc.Count = desc.ArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    auto eligible = [&]() { return is_pyrowave_source_valid(image, desc, 9, 3, 18, 10); };
    EXPECT_TRUE(eligible()); // Baseline owned images need no private provenance/identity.
    image.borrowed_vdd_texture = image.borrowed_vdd_frame = image.frame_desc.borrowed = true;
    EXPECT_TRUE(eligible()); // Borrowed frames use the same semantic admission checks.
    image.borrowed_vdd_texture = image.borrowed_vdd_frame = image.frame_desc.borrowed = false;
    image.frame_desc.adapter_luid = 9;
    image.frame_desc.source_generation = 3;
    EXPECT_TRUE(eligible());
    image.frame_desc.adapter_luid = 8;
    EXPECT_FALSE(eligible());
    image.frame_desc.adapter_luid = 9;
    image.frame_desc.source_generation = 4;
    EXPECT_FALSE(eligible());
    image.frame_desc.source_generation = 3;
    EXPECT_TRUE(is_pyrowave_source_valid(image, desc, 0, 0, 18, 10));
    for (bool *flag : { &image.blank, &image.dummy, &image.linear_gamma }) {
      *flag = true;
      EXPECT_FALSE(eligible());
      *flag = false;
    }
    for (auto domain : { platf::frame_domain_e::unknown, platf::frame_domain_e::linear_scrgb,
           platf::frame_domain_e::pq_bt2020, platf::frame_domain_e::hlg_bt2020 }) {
      image.frame_desc.domain = domain;
      EXPECT_FALSE(eligible());
    }
    image.frame_desc.domain = platf::frame_domain_e::sdr_rec709;
    image.frame_desc.encoding = platf::pixel_encoding_class_e::float16;
    EXPECT_FALSE(eligible());
    image.frame_desc.encoding = platf::pixel_encoding_class_e::unorm8;
    for (auto format : { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
           DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_NV12, DXGI_FORMAT_P010 }) {
      desc.Format = format;
      EXPECT_FALSE(eligible());
    }
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    EXPECT_TRUE(eligible());
    for (UINT *value : { &desc.Width, &desc.Height, &desc.ArraySize, &desc.MipLevels, &desc.SampleDesc.Count }) {
      const auto original = *value;
      *value = original + 1;
      EXPECT_FALSE(eligible());
      *value = original;
    }
    EXPECT_FALSE(is_pyrowave_source_valid(image, desc, 9, 3, 0, 10));
    EXPECT_FALSE(is_pyrowave_source_valid(image, desc, 9, 3, 17, 10));
    CloseHandle(image.encoder_texture_handle);
    image.encoder_texture_handle = nullptr;
    EXPECT_FALSE(eligible());
  }
} // namespace
