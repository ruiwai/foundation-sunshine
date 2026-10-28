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

  TEST(TextureLock, ExplicitAndDefaultTimeoutsWithIdempotentLockAndScopedRelease) {
    mutex_state state;
    state.acquire = {S_OK, S_OK}; state.release = {S_OK, S_OK};
    platf::dxgi::keyed_mutex_t mutex(new scripted_mutex(state));
    {
      platf::dxgi::texture_lock_helper lock(mutex.get());
      EXPECT_TRUE(lock.lock(1000)); EXPECT_TRUE(lock.lock(1));
      EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0}));
      EXPECT_EQ(state.timeouts, (std::vector<DWORD> {1000}));
      EXPECT_TRUE(state.release_keys.empty());
    }
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
    {
      platf::dxgi::texture_lock_helper lock(mutex.get());
      EXPECT_TRUE(lock.lock());
    }
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0, 0}));
    EXPECT_EQ(state.timeouts, (std::vector<DWORD> {1000, INFINITE}));
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0, 0}));
    mutex.reset(); EXPECT_TRUE(state.destroyed);
  }

  TEST(TextureLock, FailedAndNullAcquiresNeverRelease) {
    for (HRESULT result : {static_cast<HRESULT>(WAIT_TIMEOUT), static_cast<HRESULT>(WAIT_ABANDONED), E_FAIL}) {
      mutex_state state;
      state.acquire = {result};
      platf::dxgi::keyed_mutex_t mutex(new scripted_mutex(state));
      {
        platf::dxgi::texture_lock_helper lock(mutex.get());
        EXPECT_FALSE(lock.lock(1000)); EXPECT_FALSE(lock.locked);
      }
      EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0}));
      EXPECT_EQ(state.timeouts, (std::vector<DWORD> {1000}));
      EXPECT_TRUE(state.release_keys.empty());
      mutex.reset(); EXPECT_TRUE(state.destroyed);
    }
    platf::dxgi::texture_lock_helper lock(nullptr);
    EXPECT_FALSE(lock.lock()); EXPECT_FALSE(lock.locked);
  }

  TEST(PendingPyrowaveSource, ValidatesHoldsAndCompletesExactlyOnceWithReplayNoOp) {
    mutex_state state, rejected_state;
    platf::dxgi::keyed_mutex_t mutex(new scripted_mutex(state)), rejected_mutex(new scripted_mutex(rejected_state));
    auto img = std::make_shared<img_d3d_t>(), rejected_img = std::make_shared<img_d3d_t>();
    img->pyrowave_ddx_frame = true; img->encoder_release_key = 7;
    std::weak_ptr<img_d3d_t> weak = img, rejected_weak = rejected_img;
    platf::dxgi::pending_pyrowave_source_t lease;
    EXPECT_FALSE(lease.pending()); EXPECT_TRUE(lease.complete());
    EXPECT_FALSE(lease.hold(nullptr, mutex.get())); EXPECT_FALSE(lease.hold(img, nullptr));
    EXPECT_FALSE(lease.pending());
    // Caller owns the mutex; the lease itself must not acquire or submit GPU work.
    ASSERT_EQ(mutex->AcquireSync(0, 16), S_OK);
    ASSERT_TRUE(lease.hold(img, mutex.get())); EXPECT_TRUE(lease.pending());
    EXPECT_FALSE(lease.hold(rejected_img, rejected_mutex.get()));
    rejected_img.reset(); rejected_mutex.reset();
    EXPECT_TRUE(rejected_weak.expired()); EXPECT_TRUE(rejected_state.destroyed);
    EXPECT_TRUE(rejected_state.acquire_keys.empty()); EXPECT_TRUE(rejected_state.release_keys.empty());
    img.reset(); mutex.reset();
    EXPECT_FALSE(weak.expired()); EXPECT_FALSE(state.destroyed);
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0})); EXPECT_TRUE(state.release_keys.empty());
    EXPECT_TRUE(lease.complete()); EXPECT_FALSE(lease.pending());
    EXPECT_TRUE(weak.expired()); EXPECT_TRUE(state.destroyed);
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {7}));
    EXPECT_TRUE(lease.complete()); // Cached-plane replay does not release the old source twice.
    EXPECT_EQ(state.release_keys.size(), 1u);
  }

  TEST(PendingPyrowaveSource, FailedCompletionRetainsImageAndMutexUntilSuccessfulRetry) {
    mutex_state state;
    state.release = {E_FAIL, S_FALSE, S_OK};
    platf::dxgi::keyed_mutex_t mutex(new scripted_mutex(state));
    auto img = std::make_shared<img_d3d_t>(); img->pyrowave_cursor_composited = true;
    std::weak_ptr<img_d3d_t> weak = img;
    platf::dxgi::pending_pyrowave_source_t lease;
    ASSERT_EQ(mutex->AcquireSync(0, 16), S_OK);
    ASSERT_TRUE(lease.hold(img, mutex.get()));
    img.reset(); mutex.reset();
    for (int attempt = 0; attempt < 2; ++attempt) {
      EXPECT_FALSE(lease.complete()); EXPECT_TRUE(lease.pending());
      ASSERT_FALSE(weak.expired()); EXPECT_FALSE(state.destroyed);
      EXPECT_TRUE(weak.lock()->pyrowave_cursor_composited);
    }
    EXPECT_TRUE(lease.complete()); EXPECT_FALSE(lease.pending());
    EXPECT_TRUE(weak.expired()); EXPECT_TRUE(state.destroyed);
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0}));
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0, 0, 0}));
  }

  TEST(PendingPyrowaveSource, AbortDestructorDropsReferencesWithoutUnlocking) {
    mutex_state state;
    platf::dxgi::keyed_mutex_t mutex(new scripted_mutex(state));
    auto img = std::make_shared<img_d3d_t>(); img->pyrowave_ddx_frame = true;
    std::weak_ptr<img_d3d_t> weak = img;
    {
      platf::dxgi::pending_pyrowave_source_t lease;
      ASSERT_EQ(mutex->AcquireSync(0, 16), S_OK);
      ASSERT_TRUE(lease.hold(img, mutex.get()));
      img.reset(); mutex.reset();
      EXPECT_FALSE(weak.expired()); EXPECT_FALSE(state.destroyed);
    }
    EXPECT_TRUE(weak.expired()); EXPECT_TRUE(state.destroyed);
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0})); EXPECT_TRUE(state.release_keys.empty());
  }

  TEST(PendingPyrowaveSource, BorrowedCompletionReturnsProducerSlotAndClearsAccounting) {
    mutex_state state;
    auto img = std::make_shared<img_d3d_t>();
    auto counter = std::make_shared<std::atomic<UINT64>>(1);
    img->borrowed_vdd_texture = img->borrowed_vdd_frame = true;
    img->encoder_acquire_key = 2; img->producer_release_key = 0;
    img->borrowed_vdd_inflight_counter = counter;
    img->borrowed_vdd_mutex.reset(new scripted_mutex(state));
    platf::dxgi::pending_pyrowave_source_t lease;
    ASSERT_EQ(img->borrowed_vdd_mutex->AcquireSync(2, 16), S_OK);
    ASSERT_TRUE(lease.hold(img, img->borrowed_vdd_mutex.get()));
    EXPECT_TRUE(lease.complete()); EXPECT_FALSE(lease.pending());
    EXPECT_FALSE(img->borrowed_vdd_frame); EXPECT_EQ(counter->load(), 0u);
    EXPECT_TRUE(state.destroyed); EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
    EXPECT_TRUE(lease.complete()); EXPECT_EQ(state.release_keys.size(), 1u);
  }

  class OwnedSourceRelease: public testing::Test {
  protected:
    mutex_state state;
    platf::dxgi::keyed_mutex_t mutex {new scripted_mutex(state)};
    img_d3d_t img;
  };

  TEST_F(OwnedSourceRelease, CursorSuccessConsumesFreshnessAndRejectsRepeat) {
    img.pyrowave_cursor_composited = true;
    img.encoder_release_key = 7;
    state.acquire = {S_OK, S_OK};
    ASSERT_EQ(mutex->AcquireSync(0, 16), S_OK);
    EXPECT_TRUE(img.release_pyrowave_source_after_convert(mutex.get()));
    EXPECT_FALSE(img.pyrowave_cursor_composited); EXPECT_FALSE(img.pyrowave_ddx_frame);
    ASSERT_EQ(mutex->AcquireSync(0, 16), S_OK);
    EXPECT_FALSE(img.release_pyrowave_source_after_convert(mutex.get()));
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {7}));
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0, 0}));
  }

  TEST_F(OwnedSourceRelease, ReleaseFailurePreservesCursorFreshness) {
    img.pyrowave_cursor_composited = true;
    ASSERT_EQ(mutex->AcquireSync(0, 16), S_OK);
    for (HRESULT result : {E_FAIL, S_FALSE}) {
      state.release = {result};
      EXPECT_FALSE(img.release_pyrowave_source_after_convert(mutex.get()));
      EXPECT_TRUE(img.pyrowave_cursor_composited); EXPECT_FALSE(img.pyrowave_ddx_frame);
    }
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0, 0}));
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0}));
  }

  TEST_F(OwnedSourceRelease, NullUnknownMixedAndBorrowedTextureProvenanceNeverRelease) {
    img.pyrowave_cursor_composited = true;
    EXPECT_FALSE(img.release_pyrowave_source_after_convert(nullptr));
    EXPECT_TRUE(img.pyrowave_cursor_composited);
    ASSERT_EQ(mutex->AcquireSync(0, 16), S_OK);
    img.pyrowave_cursor_composited = false;
    EXPECT_FALSE(img.release_pyrowave_source_after_convert(mutex.get()));
    img.pyrowave_ddx_frame = img.pyrowave_cursor_composited = true;
    EXPECT_FALSE(img.release_pyrowave_source_after_convert(mutex.get()));
    EXPECT_TRUE(img.pyrowave_ddx_frame); EXPECT_TRUE(img.pyrowave_cursor_composited);
    img.pyrowave_cursor_composited = false;
    img.borrowed_vdd_texture = true;
    EXPECT_FALSE(img.release_pyrowave_source_after_convert(mutex.get()));
    EXPECT_TRUE(img.pyrowave_ddx_frame); EXPECT_TRUE(state.release_keys.empty());
    img.pyrowave_ddx_frame = false; img.pyrowave_cursor_composited = true;
    EXPECT_FALSE(img.release_pyrowave_source_after_convert(mutex.get()));
    EXPECT_TRUE(img.pyrowave_cursor_composited); EXPECT_TRUE(state.release_keys.empty());
  }

  TEST_F(OwnedSourceRelease, DdxRepeatWithIndependentAcquiresRetainsIdentity) {
    img.pyrowave_ddx_frame = true;
    img.id = 17; img.frame_desc.source_generation = 23;
    state.acquire = {S_OK, S_OK}; state.release = {S_OK, S_OK};
    for (int conversion = 0; conversion < 2; ++conversion) {
      // API ownership precondition only; no scripted outcome proves GPU completion.
      ASSERT_EQ(mutex->AcquireSync(0, 16), S_OK);
      EXPECT_TRUE(img.release_pyrowave_source_after_convert(mutex.get()));
      EXPECT_TRUE(img.pyrowave_ddx_frame); EXPECT_FALSE(img.pyrowave_cursor_composited);
      EXPECT_EQ(img.id, 17u); EXPECT_EQ(img.frame_desc.source_generation, 23u);
    }
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {0, 0}));
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0, 0}));
  }

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
    reject(img->pyrowave_cursor_composited, true);
    reject(img->pyrowave_ddx_frame, true);
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

  TEST(PyrowaveSourceAdmission, OnlyFreshMarkedOwnedCursorCopiesAreAccepted) {
    img_d3d_t img;
    auto &frame = img.frame_desc;
    img.blank = false;
    img.pyrowave_cursor_composited = true;
    frame.adapter_luid = 17; frame.source_generation = 23;
    frame.encoding = platf::pixel_encoding_class_e::unorm8;
    frame.domain = platf::frame_domain_e::sdr_rec709;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 1920; desc.Height = 1080;
    desc.SampleDesc.Count = desc.ArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    const auto valid = [&] {
      return platf::dxgi::is_pyrowave_source_valid(img, desc, 17, 23, 1920, 1080);
    };
    const auto reject = [&](auto &field, auto bad) {
      const auto saved = field;
      field = bad;
      EXPECT_FALSE(valid());
      field = saved;
      EXPECT_TRUE(valid());
    };
    EXPECT_TRUE(valid());
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; EXPECT_TRUE(valid());
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    reject(img.pyrowave_cursor_composited, false); // Arbitrary owned images are not sources.
    reject(img.pyrowave_ddx_frame, true); // Owned provenance must not be mixed.
    reject(img.borrowed_vdd_texture, true); reject(img.borrowed_vdd_frame, true);
    reject(frame.borrowed, true);
    img.borrowed_vdd_texture = img.borrowed_vdd_frame = frame.borrowed = true;
    img.encoder_acquire_key = 2;
    EXPECT_FALSE(valid()); // Even a complete borrowed contract cannot carry the owned marker.
    img.borrowed_vdd_texture = img.borrowed_vdd_frame = frame.borrowed = false;
    img.encoder_acquire_key = 0;
    for (UINT64 bad : {UINT64 {1}, UINT64 {2}}) {
      reject(img.encoder_acquire_key, bad); reject(img.encoder_release_key, bad);
      reject(img.producer_release_key, bad);
    }
    reject(img.blank, true); reject(img.dummy, true); reject(img.linear_gamma, true);
    reject(frame.adapter_luid, std::uint64_t {18}); reject(frame.adapter_luid, std::uint64_t {0});
    reject(frame.source_generation, std::uint64_t {24}); reject(frame.source_generation, std::uint64_t {0});
    reject(frame.encoding, platf::pixel_encoding_class_e::float16);
    reject(frame.domain, platf::frame_domain_e::unknown);
    reject(desc.Width, UINT {0}); reject(desc.Width, UINT {1918});
    reject(desc.Height, UINT {0}); reject(desc.Height, UINT {1078});
    reject(desc.SampleDesc.Count, UINT {2}); reject(desc.ArraySize, UINT {2}); reject(desc.MipLevels, UINT {2});
    reject(desc.Format, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    EXPECT_TRUE(valid());
    img.pyrowave_cursor_composited = false; // Successful conversion consumes provenance.
    EXPECT_FALSE(valid());
  }

  TEST(PyrowaveSourceAdmission, OwnedDdxIdentityAllowsRepeatedAdmissionWithoutFreshCursorMarker) {
    img_d3d_t img;
    auto &frame = img.frame_desc;
    img.blank = false;
    img.pyrowave_ddx_frame = true;
    frame.adapter_luid = 17; frame.source_generation = 23;
    frame.encoding = platf::pixel_encoding_class_e::unorm8;
    frame.domain = platf::frame_domain_e::sdr_rec709;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 1920; desc.Height = 1080;
    desc.SampleDesc.Count = desc.ArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    const auto valid = [&] {
      return platf::dxgi::is_pyrowave_source_valid(img, desc, 17, 23, 1920, 1080);
    };
    const auto reject = [&](auto &field, auto bad) {
      const auto saved = field;
      field = bad;
      EXPECT_FALSE(valid());
      field = saved;
      EXPECT_TRUE(valid());
    };
    EXPECT_TRUE(valid()); EXPECT_TRUE(valid()); // Admission is not single-use freshness.
    EXPECT_TRUE(img.pyrowave_ddx_frame); EXPECT_FALSE(img.pyrowave_cursor_composited);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; EXPECT_TRUE(valid());
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    reject(img.pyrowave_ddx_frame, false); // Unmarked owned images are not admitted.
    reject(img.pyrowave_cursor_composited, true); // XOR, not interchangeable mixed markers.
    reject(img.borrowed_vdd_texture, true); reject(img.borrowed_vdd_frame, true);
    reject(frame.borrowed, true);
    img.borrowed_vdd_texture = img.borrowed_vdd_frame = frame.borrowed = true;
    img.encoder_acquire_key = 2;
    EXPECT_FALSE(valid()); // Even complete borrowed VDD provenance cannot carry DDX identity.
    img.borrowed_vdd_texture = img.borrowed_vdd_frame = frame.borrowed = false;
    img.encoder_acquire_key = 0;
    for (UINT64 bad : {UINT64 {1}, UINT64 {2}}) {
      reject(img.encoder_acquire_key, bad); reject(img.encoder_release_key, bad);
      reject(img.producer_release_key, bad);
    }
    reject(img.blank, true); reject(img.dummy, true); reject(img.linear_gamma, true);
    reject(frame.adapter_luid, std::uint64_t {18}); reject(frame.adapter_luid, std::uint64_t {0});
    reject(frame.source_generation, std::uint64_t {24}); reject(frame.source_generation, std::uint64_t {0});
    reject(frame.encoding, platf::pixel_encoding_class_e::float16);
    for (auto bad : {platf::frame_domain_e::unknown, platf::frame_domain_e::linear_scrgb,
                    platf::frame_domain_e::pq_bt2020, platf::frame_domain_e::hlg_bt2020})
      reject(frame.domain, bad);
    reject(desc.Width, UINT {0}); reject(desc.Width, UINT {1918});
    reject(desc.Height, UINT {0}); reject(desc.Height, UINT {1078});
    for (UINT bad : {0u, 2u}) {
      reject(desc.SampleDesc.Count, bad); reject(desc.ArraySize, bad); reject(desc.MipLevels, bad);
    }
    for (auto bad : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT,
                    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB})
      reject(desc.Format, bad);
    EXPECT_TRUE(valid()); EXPECT_TRUE(img.pyrowave_ddx_frame);
  }

  TEST_F(SourceLease, PyrowaveReleaseRoutesBorrowedAndCleansCounterExactlyOnce) {
    // Keep an owned COM reference while the image consumes its borrowed reference.
    img->borrowed_vdd_mutex->AddRef();
    platf::dxgi::keyed_mutex_t mutex(img->borrowed_vdd_mutex.get());
    ASSERT_EQ(mutex->AcquireSync(2, 16), S_OK);
    EXPECT_TRUE(img->release_pyrowave_source_after_convert(mutex.get()));
    expect_frame(false);
    EXPECT_TRUE(img->release_borrowed_vdd_after_convert(nullptr));
    img.reset(); mutex.reset();
    EXPECT_EQ(state.release_keys, (std::vector<UINT64> {0}));
    EXPECT_EQ(state.acquire_keys, (std::vector<UINT64> {2}));
    EXPECT_EQ(counter->load(), 0u); EXPECT_TRUE(state.destroyed);
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
