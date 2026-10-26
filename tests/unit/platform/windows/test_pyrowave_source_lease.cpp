#include <gtest/gtest.h>
#include <deque>
#include <vector>

#include "src/platform/windows/display_vram_internal.h"

namespace {
  using platf::dxgi::img_d3d_t;
  // Scripted API outcomes only: this does not model GPU ordering or device loss.
  struct mutex_state {
    std::deque<HRESULT> acquire {S_OK}, release {S_OK};
    std::vector<UINT64> acquire_keys, release_keys;
    std::vector<DWORD> timeouts;
    bool destroyed = false;
  };

  class scripted_mutex final: public IDXGIKeyedMutex {
    mutex_state &state;
    std::atomic<ULONG> refs {1};
    static HRESULT next(std::deque<HRESULT> &script) {
      if (script.empty()) return E_FAIL;
      const auto result = script.front(); script.pop_front(); return result;
    }
  public:
    explicit scripted_mutex(mutex_state &s): state(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
      if (!out) return E_POINTER;
      *out = nullptr;
      if (iid != __uuidof(IUnknown) && iid != __uuidof(IDXGIKeyedMutex)) return E_NOINTERFACE;
      *out = static_cast<IDXGIKeyedMutex *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
      const ULONG remaining = --refs;
      if (!remaining) { state.destroyed = true; delete this; }
      return remaining;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AcquireSync(UINT64 key, DWORD timeout) override {
      state.acquire_keys.push_back(key); state.timeouts.push_back(timeout); return next(state.acquire);
    }
    HRESULT STDMETHODCALLTYPE ReleaseSync(UINT64 key) override {
      state.release_keys.push_back(key); return next(state.release);
    }
  };

  class SourceLease: public testing::Test {
  protected:
    // State and counter outlive both the image and its owned COM reference.
    mutex_state state;
    std::shared_ptr<std::atomic<UINT64>> counter = std::make_shared<std::atomic<UINT64>>(1);
    std::unique_ptr<img_d3d_t> img = std::make_unique<img_d3d_t>();
    void SetUp() override {
      img->borrowed_vdd_texture = img->borrowed_vdd_frame = true;
      img->borrowed_vdd_slot = 1;
      img->encoder_acquire_key = 2;
      img->encoder_release_key = img->producer_release_key = 0;
      img->borrowed_vdd_inflight_counter = counter;
      img->borrowed_vdd_mutex.reset(new scripted_mutex(state));
    }
    ~SourceLease() override {
      if (img && img->borrowed_vdd_frame) {
        state.acquire = {S_OK}; state.release = {S_OK};
        img->abandon_borrowed_vdd_frame(false, 16);
      }
    }
    void expect_frame(bool pending) {
      EXPECT_EQ(img->borrowed_vdd_frame, pending);
      EXPECT_TRUE(img->borrowed_vdd_texture); // Borrowed-texture identity survives consumption.
      EXPECT_EQ(img->borrowed_vdd_slot, pending ? 1u : 0u);
      EXPECT_EQ(img->encoder_acquire_key, pending ? 2u : 0u);
      EXPECT_EQ(img->encoder_release_key, 0u); EXPECT_EQ(img->producer_release_key, 0u);
      EXPECT_EQ(counter->load(), pending ? 1u : 0u);
      EXPECT_EQ(img->borrowed_vdd_inflight_counter, pending ? counter : nullptr);
      EXPECT_EQ(static_cast<bool>(img->borrowed_vdd_mutex), pending);
    }
  };

  TEST_F(SourceLease, SourceAdmissionMatchesPerFrameContractWithoutGpu) {
    auto &frame = img->frame_desc;
    img->blank = img->dummy = img->linear_gamma = false;
    frame.borrowed = true;
    frame.adapter_luid = 17; frame.source_generation = 23;
    frame.encoding = platf::pixel_encoding_class_e::unorm8;
    frame.domain = platf::frame_domain_e::sdr_rec709;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 1920; desc.Height = 1080;
    desc.SampleDesc.Count = desc.ArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    const auto valid = [&] {
      return platf::dxgi::is_pyrowave_source_valid(*img, desc, 17, 23, 1920, 1080);
    };
    const auto reject = [&](auto &field, auto bad) {
      const auto saved = field;
      field = bad;
      EXPECT_FALSE(valid());
      field = saved;
      expect_frame(true);
      EXPECT_TRUE(state.acquire_keys.empty()); EXPECT_TRUE(state.release_keys.empty());
      EXPECT_TRUE(state.timeouts.empty()); EXPECT_FALSE(state.destroyed);
    };
    EXPECT_TRUE(valid());
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; EXPECT_TRUE(valid());
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    reject(img->blank, true); reject(img->dummy, true);
    reject(img->borrowed_vdd_texture, false); reject(img->borrowed_vdd_frame, false);
    reject(img->linear_gamma, true); reject(frame.borrowed, false);
    reject(frame.adapter_luid, std::uint64_t {18}); reject(frame.adapter_luid, std::uint64_t {0});
    reject(frame.source_generation, std::uint64_t {24}); reject(frame.source_generation, std::uint64_t {0});
    for (auto bad : {platf::pixel_encoding_class_e::automatic, platf::pixel_encoding_class_e::float16})
      reject(frame.encoding, bad);
    for (auto bad : {platf::frame_domain_e::unknown, platf::frame_domain_e::linear_scrgb,
                    platf::frame_domain_e::pq_bt2020, platf::frame_domain_e::hlg_bt2020})
      reject(frame.domain, bad);
    reject(img->encoder_acquire_key, UINT64 {0}); reject(img->encoder_acquire_key, UINT64 {1});
    reject(img->producer_release_key, UINT64 {2});
    reject(desc.Width, UINT {0}); reject(desc.Width, UINT {1918});
    reject(desc.Height, UINT {0}); reject(desc.Height, UINT {1078});
    for (UINT bad : {0u, 2u}) {
      reject(desc.SampleDesc.Count, bad); reject(desc.ArraySize, bad); reject(desc.MipLevels, bad);
    }
    for (auto bad : {DXGI_FORMAT_UNKNOWN, static_cast<DXGI_FORMAT>(0x7fffffff),
                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB})
      reject(desc.Format, bad);
    EXPECT_TRUE(valid()); expect_frame(true);
    EXPECT_TRUE(img->release_borrowed_vdd_after_convert(img->borrowed_vdd_mutex.get()));
    EXPECT_FALSE(valid()); expect_frame(false);
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
    EXPECT_TRUE(state.acquire_keys.empty()); EXPECT_TRUE(state.timeouts.empty());
  }

  TEST_F(SourceLease, AlreadyAcquiredReleaseIsExactlyOnce) {
    EXPECT_TRUE(img->release_borrowed_vdd_after_convert(img->borrowed_vdd_mutex.get()));
    expect_frame(false);
    EXPECT_TRUE(img->release_borrowed_vdd_after_convert(nullptr));
    EXPECT_TRUE(img->abandon_borrowed_vdd_frame(false, 16));
    img.reset();
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
    EXPECT_TRUE(state.acquire_keys.empty()); EXPECT_TRUE(state.destroyed);
    EXPECT_EQ(counter->load(), 0u);
  }

  TEST_F(SourceLease, AbandonRequiresExactAcquireSuccess) {
    state.acquire = {static_cast<HRESULT>(WAIT_TIMEOUT), static_cast<HRESULT>(WAIT_ABANDONED), E_FAIL, S_OK};
    for (int attempt = 0; attempt < 3; ++attempt) {
      EXPECT_FALSE(img->abandon_borrowed_vdd_frame(false, 16));
      expect_frame(true); EXPECT_TRUE(state.release_keys.empty());
    }
    EXPECT_TRUE(img->abandon_borrowed_vdd_frame(false, 16));
    expect_frame(false);
    EXPECT_TRUE(img->abandon_borrowed_vdd_frame(false, 16));
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {2, 2, 2, 2}));
    EXPECT_EQ(state.timeouts, (std::vector<DWORD> {16, 16, 16, 16}));
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
    EXPECT_TRUE(state.destroyed);
  }

  TEST_F(SourceLease, FailedReleaseAndDestructorTimeoutDoNotComplete) {
    state.release = {E_FAIL}; state.acquire = {static_cast<HRESULT>(WAIT_TIMEOUT)};
    EXPECT_FALSE(img->release_borrowed_vdd_after_convert(img->borrowed_vdd_mutex.get()));
    expect_frame(true);
    img.reset();
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {2}));
    EXPECT_EQ(state.timeouts, (std::vector<DWORD> {16}));
    EXPECT_EQ(counter->load(), 1u); EXPECT_TRUE(state.destroyed);
  }

  TEST_F(SourceLease, DestructorUsesBoundedAcquireAndNeverFalselyCompletes) {
    for (HRESULT result : {S_OK, static_cast<HRESULT>(WAIT_TIMEOUT), static_cast<HRESULT>(WAIT_ABANDONED), E_FAIL}) {
      // A fresh real image and COM object per outcome; no raw pointer survives reset.
      if (!img) { state = mutex_state {}; counter->store(1); img = std::make_unique<img_d3d_t>(); SetUp(); }
      state.acquire = {result};
      img.reset();
      EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {2}));
      EXPECT_EQ(state.timeouts, (std::vector<DWORD> {16})); // Argument bound, not GPU elapsed time.
      EXPECT_EQ(state.release_keys, result == S_OK ? std::vector<UINT64> {0} : std::vector<UINT64> {});
      EXPECT_EQ(counter->load(), result == S_OK ? 0u : 1u); EXPECT_TRUE(state.destroyed);
    }
  }

  TEST_F(SourceLease, AlreadyAcquiredReleaseRejectsSFalse) {
    state.release = {S_FALSE};
    EXPECT_FALSE(img->release_borrowed_vdd_after_convert(img->borrowed_vdd_mutex.get()));
    expect_frame(true);
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
    EXPECT_TRUE(state.acquire_keys.empty()); EXPECT_TRUE(state.timeouts.empty());
  }

  TEST_F(SourceLease, AbandonReleaseRejectsSFalse) {
    state.release = {S_FALSE};
    EXPECT_FALSE(img->abandon_borrowed_vdd_frame(false, 16));
    expect_frame(true);
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {2}));
    EXPECT_EQ(state.timeouts, (std::vector<DWORD> {16}));
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
  }

  TEST_F(SourceLease, MissingBorrowedMutexNeverCompletesLease) {
    img->borrowed_vdd_mutex.reset();
    EXPECT_FALSE(img->abandon_borrowed_vdd_frame(false, 16));
    EXPECT_TRUE(img->borrowed_vdd_frame); EXPECT_TRUE(img->borrowed_vdd_texture);
    EXPECT_FALSE(img->borrowed_vdd_mutex);
    EXPECT_EQ(img->borrowed_vdd_slot, 1u); EXPECT_EQ(img->encoder_acquire_key, 2u);
    EXPECT_EQ(img->encoder_release_key, 0u); EXPECT_EQ(img->producer_release_key, 0u);
    EXPECT_EQ(img->borrowed_vdd_inflight_counter, counter); EXPECT_EQ(counter->load(), 1u);
    img.reset();
    EXPECT_EQ(counter->load(), 1u); EXPECT_TRUE(state.destroyed);
    EXPECT_TRUE(state.acquire_keys.empty()); EXPECT_TRUE(state.timeouts.empty());
    EXPECT_TRUE(state.release_keys.empty());
  }

  TEST_F(SourceLease, MissingEncoderMutexKeepsLeaseForSafeCleanup) {
    EXPECT_FALSE(img->release_borrowed_vdd_after_convert(nullptr));
    expect_frame(true);
    EXPECT_TRUE(state.acquire_keys.empty()); EXPECT_TRUE(state.release_keys.empty());
    EXPECT_FALSE(state.destroyed);
    EXPECT_TRUE(img->abandon_borrowed_vdd_frame(false, 16));
    expect_frame(false);
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {2}));
    EXPECT_EQ(state.timeouts, (std::vector<DWORD> {16}));
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
  }
} // namespace
