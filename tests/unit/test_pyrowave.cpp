/**
 * @file tests/unit/test_pyrowave.cpp
 * @brief Pure-data and state-transition tests for the PyroWave foundation.
 */
#include "src/pyrowave/bounded_wait.h"
#include "src/pyrowave/capabilities.h"
#include "src/pyrowave/capture_lifecycle.h"
#include "src/pyrowave/config.h"
#include "src/pyrowave/encode_guard.h"
#include "src/pyrowave/plane_ring.h"
#include "src/pyrowave/gpu_interop.h"
#include "src/pyrowave/packet.h"
#include "src/pyrowave/packetizer.h"
#include "src/pyrowave/session.h"
#include "src/pyrowave/signal_contract.h"
#include "src/pyrowave/types.h"
#include "src/pyrowave/video_admission.h"
#include "src/pyrowave/runtime.h"
#include "src/pyrowave/dynamic_hdr.h"
#include "third-party/moonlight-common-c/tests/PyrowaveDynamicHdrFixtures.h"
#ifdef _WIN32
#include <vulkan/vulkan.h>
#include "src/platform/windows/pyrowave/color_metadata.h"
#include "src/platform/windows/pyrowave/rate_control.h"
#include "src/platform/windows/pyrowave/transport.h"
#endif

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
#include "third-party/moonlight-common-c/src/PyrowaveReassembly.h"
}

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

  using capture_clock = std::chrono::steady_clock;
  using capture_result = pyrowave::capture_wait_result;
  using namespace std::chrono_literals;

  struct capture_signal {
    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    std::once_flag notified;
    void notify() { std::call_once(notified, [&]() { promise.set_value(); }); }
    bool wait() { return future.wait_for(2s) == std::future_status::ready; }
  };

  struct capture_state {
    std::atomic<bool> running { true };
    std::atomic<int> stops { 0 }, work { 0 }, exits { 0 };
    std::mutex mutex;
    std::condition_variable cv;
    void stop() {
      {
        std::lock_guard lock(mutex);
        running = false;
        ++stops;
      }
      cv.notify_all();
    }
    bool wait_stopped() {
      std::unique_lock lock(mutex);
      return cv.wait_for(lock, 2s, [&]() { return stops.load() != 0; });
    }
  };

  struct capture_resource {
    std::atomic<int> &destroyed;
    explicit capture_resource(std::atomic<int> &count): destroyed(count) {}
    ~capture_resource() { ++destroyed; }
  };

  TEST(PyrowaveCaptureLifecycleTest, PreCancelledStartSkipsWorkAndStopsOutput) {
    capture_state state;
    state.running = false;
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); }, [&]() { return state.running.load(); },
        [&]() { ++state.work; }
      };
      EXPECT_TRUE(state.wait_stopped());
    }
    EXPECT_EQ(state.work, 0);
    EXPECT_EQ(state.stops, 2);
    EXPECT_FALSE(state.running);
  }

  TEST(PyrowaveCaptureLifecycleTest, NormalCompletionStopsOutputAndJoins) {
    capture_state state;
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); }, [&]() { return state.running.load(); },
        [&]() { ++state.work; ++state.exits; }
      };
      EXPECT_TRUE(state.wait_stopped());
      EXPECT_EQ(state.stops, 1);
    }
    EXPECT_EQ(state.work, 1);
    EXPECT_EQ(state.exits, 1);
    EXPECT_EQ(state.stops, 2);
  }

  TEST(PyrowaveCaptureLifecycleTest, ThrowingWorkStopsOutputReportsAndJoins) {
    capture_state state;
    std::atomic<int> destroyed { 0 };
    std::exception_ptr reported;
    bool stopped_at_report = false;
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); }, []() { return true; },
        [&, resource = std::make_shared<capture_resource>(destroyed)]() {
          ++state.work;
          throw std::runtime_error("work failed");
        },
        [&](std::exception_ptr failure) {
          stopped_at_report = !state.running && state.stops >= 1;
          reported = failure;
        }
      };
      EXPECT_TRUE(state.wait_stopped());
    }
    EXPECT_EQ(state.work, 1);
    EXPECT_EQ(state.stops, 2);
    EXPECT_FALSE(state.running);
    EXPECT_TRUE(stopped_at_report);
    EXPECT_EQ(destroyed, 1);
    ASSERT_TRUE(reported);
    try {
      std::rethrow_exception(reported);
    } catch (const std::runtime_error &e) {
      EXPECT_STREQ(e.what(), "work failed");
    }
  }

  TEST(PyrowaveCaptureLifecycleTest, ThrowingRunningSkipsWorkStopsOutputReportsAndJoins) {
    capture_state state;
    std::atomic<int> destroyed { 0 };
    std::exception_ptr reported;
    bool stopped_at_report = false;
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); },
        [resource = std::make_shared<capture_resource>(destroyed)]() -> bool {
          throw std::logic_error("running failed");
        },
        [&]() { ++state.work; },
        [&](std::exception_ptr failure) {
          stopped_at_report = !state.running && state.stops >= 1;
          reported = failure;
        }
      };
      EXPECT_TRUE(state.wait_stopped());
    }
    EXPECT_EQ(state.work, 0);
    EXPECT_EQ(state.stops, 2);
    EXPECT_FALSE(state.running);
    EXPECT_TRUE(stopped_at_report);
    EXPECT_EQ(destroyed, 1);
    ASSERT_TRUE(reported);
    try {
      std::rethrow_exception(reported);
    } catch (const std::logic_error &e) {
      EXPECT_STREQ(e.what(), "running failed");
    }
  }

  TEST(PyrowaveCaptureLifecycleTest, ThrowingReporterIsContainedAndWorkerJoins) {
    capture_state state;
    std::atomic<int> destroyed { 0 };
    std::exception_ptr reported;
    bool stopped_at_report = false;
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); }, []() { return true; },
        [&]() { ++state.work; throw 42; },
        [&, resource = std::make_shared<capture_resource>(destroyed)](std::exception_ptr failure) {
          stopped_at_report = !state.running && state.stops >= 1;
          reported = failure;
          throw std::runtime_error("reporter failed");
        }
      };
      EXPECT_TRUE(state.wait_stopped());
    }
    EXPECT_EQ(state.work, 1);
    EXPECT_EQ(state.stops, 2);
    EXPECT_FALSE(state.running);
    EXPECT_TRUE(stopped_at_report);
    EXPECT_EQ(destroyed, 1);
    ASSERT_TRUE(reported);
    try {
      std::rethrow_exception(reported);
    } catch (int failure) {
      EXPECT_EQ(failure, 42);
    }
  }

  TEST(PyrowaveCaptureLifecycleTest, DestructionStopsAndJoinsBeforeDependentStateDies) {
    capture_state state;
    capture_signal entered;
    std::atomic<int> destroyed { 0 };
    auto dependent = std::make_unique<capture_resource>(destroyed);
    bool saw_stop = false;
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); }, [&]() { return state.running.load(); },
        [&]() {
          ++state.work;
          entered.notify();
          saw_stop = state.wait_stopped();
          EXPECT_EQ(dependent->destroyed.load(), 0);
          ++state.exits;
        }
      };
      EXPECT_TRUE(entered.wait());
      EXPECT_EQ(state.exits, 0);
    }
    EXPECT_TRUE(saw_stop);
    EXPECT_EQ(state.exits, 1);
    EXPECT_EQ(state.stops, 2);
    EXPECT_EQ(destroyed, 0);
    dependent.reset();
    EXPECT_EQ(destroyed, 1);
  }

  TEST(PyrowaveCaptureLifecycleTest, DrainDropsQueuedReferenceButWaitsForExternalHolder) {
    capture_state state;
    capture_signal drained, release;
    std::atomic<int> destroyed { 0 }, drains { 0 };
    auto display = std::make_shared<capture_resource>(destroyed);
    std::weak_ptr<capture_resource> weak = display;
    auto external = display, queued = display;
    capture_result result = capture_result::stopped;
    auto now = capture_clock::time_point {};
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); }, [&]() { return state.running.load(); },
        [&]() {
          result = pyrowave::wait_capture_references(
            [&]() { return weak.use_count() == 1; }, [&]() { return state.running.load(); },
            [&]() { queued.reset(); ++drains; }, [&]() { return now; },
            [&]() { drained.notify(); if (!release.wait()) state.stop(); now += 20ms; }, 5s, true);
          ++state.exits;
        }
      };
      EXPECT_TRUE(drained.wait());
      EXPECT_EQ(drains, 1);
      EXPECT_EQ(weak.use_count(), 2);
      EXPECT_EQ(state.exits, 0);
      EXPECT_EQ(destroyed, 0);
      external.reset();
      release.notify();
      EXPECT_TRUE(state.wait_stopped());
    }
    EXPECT_EQ(result, capture_result::ready);
    EXPECT_EQ(state.exits, 1);
    EXPECT_EQ(state.stops, 2);
    EXPECT_FALSE(queued);
    EXPECT_EQ(destroyed, 0);
    display.reset();
    EXPECT_EQ(destroyed, 1);
  }

  TEST(PyrowaveCaptureLifecycleTest, HeldReferenceTimesOutWithoutConfiscationAndOwnerJoins) {
    capture_state state;
    std::atomic<int> destroyed { 0 };
    auto display = std::make_shared<capture_resource>(destroyed);
    std::weak_ptr<capture_resource> weak = display;
    auto external = display;
    auto now = capture_clock::time_point {};
    int drains = 0, pauses = 0;
    capture_result result = capture_result::ready;
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); }, [&]() { return state.running.load(); },
        [&]() {
          result = pyrowave::wait_capture_references(
            [&]() { return weak.use_count() == 1; }, [&]() { return state.running.load(); },
            [&]() { ++drains; }, [&]() { return now; },
            [&]() { now += 20ms; if (++pauses > 250) state.stop(); }, 5s, true);
          ++state.exits;
        }
      };
      EXPECT_TRUE(state.wait_stopped());
    }
    EXPECT_EQ(result, capture_result::timed_out);
    EXPECT_EQ(now.time_since_epoch(), 5s);
    EXPECT_EQ(drains, 250);
    EXPECT_EQ(pauses, 250);
    EXPECT_EQ(state.exits, 1);
    EXPECT_EQ(state.stops, 2);
    EXPECT_EQ(destroyed, 0);
    display.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(destroyed, 0);
    external.reset();
    EXPECT_EQ(destroyed, 1);
  }

  TEST(PyrowaveCaptureLifecycleTest, CancellationInterruptsPendingWaitAndPrecedesReady) {
    capture_state state;
    capture_signal pending, resume;
    std::atomic<int> destroyed { 0 };
    auto display = std::make_shared<capture_resource>(destroyed), external = display;
    std::weak_ptr<capture_resource> weak = display;
    capture_result result = capture_result::ready;
    int drains = 0, ready_calls = 0;
    {
      pyrowave::capture_worker owner {
        [&]() { state.stop(); }, [&]() { return state.running.load(); },
        [&]() {
          result = pyrowave::wait_capture_references(
            [&]() { ++ready_calls; return weak.use_count() == 1; }, [&]() { return state.running.load(); },
            [&]() { ++drains; }, []() { return capture_clock::now(); },
            [&]() { pending.notify(); if (!resume.wait()) state.stop(); }, 5s, true);
          ++state.exits;
        }
      };
      EXPECT_TRUE(pending.wait());
      state.stop();
      external.reset(); // Ready and stopped together: cancellation must win.
      resume.notify();
    }
    EXPECT_EQ(result, capture_result::stopped);
    EXPECT_EQ(drains, 1);
    EXPECT_EQ(ready_calls, 1);
    EXPECT_EQ(state.exits, 1);
    EXPECT_EQ(state.stops, 3);
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(pyrowave::wait_capture_references(
      [&]() { ++ready_calls; return true; }, []() { return false; },
      [&]() { ++drains; }, []() { return capture_clock::now(); }, []() {}, 5s, true), capture_result::stopped);
    EXPECT_EQ(ready_calls, 1);
    EXPECT_EQ(drains, 1);
  }

  TEST(PyrowaveCaptureLifecycleTest, UnboundedLegacyWaitIgnoresDeadlineButHonorsStop) {
    capture_state state;
    auto now = capture_clock::time_point {};
    int drains = 0, pauses = 0;
    const auto result = pyrowave::wait_capture_references(
      []() { return false; }, [&]() { return state.running.load(); },
      [&]() { ++drains; }, [&]() { return now; },
      [&]() { now += 1s; if (++pauses == 3) state.stop(); }, 20ms, false);
    EXPECT_EQ(result, capture_result::stopped);
    EXPECT_EQ(drains, 3);
    EXPECT_EQ(pauses, 3);
    EXPECT_EQ(now.time_since_epoch(), 3s);
    EXPECT_EQ(state.stops, 1);
  }

  TEST(PyrowaveIdlePolicyTest, RejectsUnverifiedSuppression) {
    static_assert(noexcept(pyrowave::validate_idle_policy(false)));
    static_assert(noexcept(pyrowave::validate_idle_policy(true)));
    static_assert(pyrowave::validate_idle_policy(false) == nullptr);
    static_assert(pyrowave::validate_idle_policy(true) != nullptr);
    EXPECT_EQ(pyrowave::validate_idle_policy(false), nullptr);
    EXPECT_STREQ(pyrowave::validate_idle_policy(true), "PyroWave requires variable_refresh_rate disabled until paired-client idle/refresh behavior is verified");
  }

  TEST(PyrowaveCursorPolicyTest, AllowsHostCursorButRejectsLocalCursor) {
    static_assert(noexcept(pyrowave::validate_cursor_policy(false, false)));
    static_assert(pyrowave::validate_cursor_policy(false, false) == nullptr);
    static_assert(pyrowave::validate_cursor_policy(true, false) == nullptr);
    static_assert(pyrowave::validate_cursor_policy(false, true) != nullptr);
    static_assert(pyrowave::validate_cursor_policy(true, true) != nullptr);
    EXPECT_EQ(pyrowave::validate_cursor_policy(false, false), nullptr);
    EXPECT_EQ(pyrowave::validate_cursor_policy(true, false), nullptr);
    EXPECT_STREQ(pyrowave::validate_cursor_policy(false, true), "PyroWave local-cursor rendering is unsupported; disable local cursor mode");
    EXPECT_STREQ(pyrowave::validate_cursor_policy(true, true), "PyroWave local-cursor rendering is unsupported; disable local cursor mode");
  }

  TEST(PyrowaveRuntimeTest, AdvertisesOnlyPrivateSdrCapabilities) {
    const auto flags = pyrowave::server_capability_flags();
    EXPECT_EQ(flags, LI_PYROWAVE_CAPABILITY_REASSEMBLY |
                     LI_PYROWAVE_CAPABILITY_FRAME_DEADLINE |
                     LI_PYROWAVE_CAPABILITY_BLOCK_AWARE_FEC |
                     LI_PYROWAVE_CAPABILITY_FRAME_METADATA |
                     LI_PYROWAVE_CAPABILITY_SDR_BT709_YUV420 |
                     LI_PYROWAVE_CAPABILITY_YUV_FULL_RANGE |
                     LI_PYROWAVE_CAPABILITY_YUV_LIMITED_RANGE);
    EXPECT_EQ(flags & (LI_PYROWAVE_CAPABILITY_HDR10_PQ_BT2020 |
                       LI_PYROWAVE_CAPABILITY_HLG_BT2020 |
                       LI_PYROWAVE_CAPABILITY_DYNAMIC_HDR_MAPPING), 0u);
  }

  static_assert(noexcept(pyrowave::validate_capture_backend(true, "vdd")));
  static_assert(pyrowave::validate_capture_backend(true, "") == nullptr);
  static_assert(pyrowave::validate_capture_backend(true, "ddx") == nullptr);
  static_assert(pyrowave::validate_capture_backend(true, "vdd") == nullptr);
  static_assert(pyrowave::validate_capture_backend(false, "") != nullptr);
  static_assert(pyrowave::validate_capture_backend(false, "ddx") != nullptr);
  static_assert(pyrowave::validate_capture_backend(false, "vdd") != nullptr);

  TEST(PyrowaveCaptureBackendTest, AutoDdxAndVddRequireDxgi) {
    for (const auto backend : { "", "ddx", "vdd" }) {
      EXPECT_EQ(pyrowave::validate_capture_backend(true, backend), nullptr);
      EXPECT_NE(pyrowave::validate_capture_backend(false, backend), nullptr);
    }
  }

  TEST(PyrowaveCaptureBackendTest, RejectsUnsupportedBackends) {
    for (const auto backend : { "wgc", "amd", "unknown", "VDD", "DDX" }) {
      EXPECT_NE(pyrowave::validate_capture_backend(true, backend), nullptr);
      EXPECT_NE(pyrowave::validate_capture_backend(false, backend), nullptr);
    }
  }

  TEST(PyrowaveCaptureBackendTest, OnlyExplicitVddUsesDirectCapture) {
    static_assert(noexcept(pyrowave::uses_direct_vdd_capture("vdd")));
    static_assert(!pyrowave::uses_direct_vdd_capture(""));
    static_assert(!pyrowave::uses_direct_vdd_capture("ddx"));
    static_assert(pyrowave::uses_direct_vdd_capture("vdd"));
    for (const auto backend : { "", "ddx", "vdd" }) {
      ASSERT_EQ(pyrowave::validate_capture_backend(true, backend), nullptr);
      EXPECT_EQ(pyrowave::uses_direct_vdd_capture(backend), std::string_view(backend) == "vdd");
    }
  }

  TEST(PyrowaveCaptureTargetTest, AcceptsOnlyUnsetOrExactDisplay) {
    static_assert(noexcept(pyrowave::validate_capture_target("display")));
    static_assert(pyrowave::validate_capture_target("") == nullptr);
    static_assert(pyrowave::validate_capture_target("display") == nullptr);
    static_assert(pyrowave::validate_capture_target("window") != nullptr);
    static_assert(pyrowave::validate_capture_target("unknown") != nullptr);
    static_assert(pyrowave::validate_capture_target("DISPLAY") != nullptr);
    static_assert(pyrowave::validate_capture_target(" display ") != nullptr);
    for (const auto target : { "", "display" }) {
      EXPECT_EQ(pyrowave::validate_capture_target(target), nullptr);
    }
    for (const auto target : { "window", "unknown", "DISPLAY", " display ", " ", "display ", " display" }) {
      EXPECT_STREQ(pyrowave::validate_capture_target(target), "PyroWave supports display capture only; set capture_target=display");
    }
  }

  pyrowave::config_t
  enabled_config() {
    pyrowave::config_t config;
    config.enabled = true;
    config.allow_experimental_client = true;
    return config;
  }

  pyrowave::device_capabilities_t
  supported_device() {
    pyrowave::device_capabilities_t device;
    device.api_version = { 101, 0, 0 };
    device.vulkan_13 = true;
    device.subgroup = true;
    device.subgroup_size_control = true;
    device.shader_int16 = true;
    device.storage_buffer_8bit = true;
    return device;
  }

  pyrowave::client_capabilities_t
  supported_client() {
    pyrowave::client_capabilities_t client;
    client.api_version = { 101, 0, 0 };
    client.pyrowave = true;
    client.reassembly = true;
    client.frame_deadline = true;
    client.block_aware_fec = true;
    return client;
  }

  std::vector<std::uint8_t>
  optional_metadata(std::size_t total_size) {
    std::vector<std::uint8_t> metadata(total_size, 0x5a);
    const auto value_size = static_cast<std::uint32_t>(total_size - 8);
    // Unknown optional TLV, with a valid length and flags.
    metadata[0] = 0x80;
    metadata[1] = 0x00;
    metadata[2] = 0x00;
    metadata[3] = LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL;
    metadata[4] = static_cast<std::uint8_t>(value_size >> 24);
    metadata[5] = static_cast<std::uint8_t>(value_size >> 16);
    metadata[6] = static_cast<std::uint8_t>(value_size >> 8);
    metadata[7] = static_cast<std::uint8_t>(value_size);
    return metadata;
  }

#ifdef _WIN32
  SS_HDR_METADATA
  hdr_source_metadata() {
    SS_HDR_METADATA source {};
    source.maxDisplayLuminance = 1000;
    source.minDisplayLuminance = 1;
    source.maxContentLightLevel = 1200;
    source.maxFrameAverageLightLevel = 200;
    return source;
  }

  platf::hdr_frame_luminance_stats_t
  hdr_scene_stats() {
    platf::hdr_frame_luminance_stats_t stats {};
    stats.valid = true;
    stats.sample_sequence = 1;
    stats.min_maxrgb = 0;
    stats.max_maxrgb = 1000;
    stats.avg_maxrgb = 100;
    stats.avg_maxrgb_pq = video::hdr_metadata::nits_to_pq(100);
    stats.percentile_1_pq = 0;
    stats.near_black_fraction = 0.05f;
    stats.near_black_stats_valid = true;
    stats.percentile_10_pq = video::hdr_metadata::nits_to_pq(10);
    stats.percentile_90_pq = video::hdr_metadata::nits_to_pq(700);
    stats.percentile_99 = 1000;
    stats.analysis_max_nits = 1000;
    const float distribution[] = { 0, 5, 10, 25, 100, 400, 700, 900, 1000 };
    std::copy(std::begin(distribution), std::end(distribution), stats.distribution_maxrgb);
    return stats;
  }
#endif

}  // namespace

#ifdef _WIN32
TEST(PyrowaveApi, UsesTheForkVersionNamespace) {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
  pyrowave_get_api_version(&major, &minor, &patch);
  EXPECT_EQ(major, 101u);
  EXPECT_EQ(minor, 0u);
  EXPECT_EQ(patch, 0u);
  const pyrowave::config_t config;
  EXPECT_EQ(config.expected_api_version.major, major);
  EXPECT_EQ(config.expected_api_version.minor, minor);
  EXPECT_EQ(config.expected_api_version.patch, patch);
}

TEST(PyrowaveApi, RejectsInvalidContextsWithoutAccessingADevice) {
  pyrowave_cpu_buffer buffer {};
  EXPECT_EQ(pyrowave_encoder_set_frame_context(nullptr, -1), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_encoder_set_frame_context(nullptr, PYROWAVE_MAX_FRAME_CONTEXTS), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_async(nullptr, &buffer, -1), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_complete(nullptr, &buffer, -1), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_async(nullptr, &buffer, PYROWAVE_MAX_FRAME_CONTEXTS), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_complete(nullptr, &buffer, PYROWAVE_MAX_FRAME_CONTEXTS), PYROWAVE_ERROR_INVALID_ARGUMENT);
}
#endif

TEST(PyrowaveGpuInteropTest, PrivateAdapterRequiresIntelPciAllowlist) {
  for (const auto device : { 0x4680u, 0xA780u, 0xA788u }) {
    EXPECT_TRUE(pyrowave::is_private_adapter(0x8086, device));
    for (const auto vendor : { 0u, 0x10deu, 0x1002u, 0xFFFFu }) {
      EXPECT_FALSE(pyrowave::is_private_adapter(vendor, device));
    }
  }
  for (const auto device : { 0u, 0xFFFFu, 0x4682u, 0x4690u, 0x46A1u,
                            0xA781u, 0xA782u, 0xA787u, 0xA789u }) {
    EXPECT_FALSE(pyrowave::is_private_adapter(0x8086, device));
  }
}

TEST(PyrowaveGpuInteropTest, ValidatesFenceCompletionWithoutAcceptingDeviceRemoval) {
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(pyrowave::is_fence_completion_valid(4, 5));
  EXPECT_TRUE(pyrowave::is_fence_completion_valid(5, 5));
  EXPECT_TRUE(pyrowave::is_fence_completion_valid(6, 5));
  EXPECT_TRUE(pyrowave::is_fence_completion_valid(0, 0));
  EXPECT_FALSE(pyrowave::is_fence_completion_valid(0, 1));
  EXPECT_FALSE(pyrowave::is_fence_completion_valid(max, 0));
  EXPECT_FALSE(pyrowave::is_fence_completion_valid(max, 5));
  EXPECT_FALSE(pyrowave::is_fence_completion_valid(max, max));
  EXPECT_TRUE(pyrowave::is_fence_completion_valid(max - 1, max - 1));
}

TEST(PyrowavePlaneRingTest, AlternatesAndRefreshesOnlyAfterRelease) {
  pyrowave::plane_ring_t ring;
  EXPECT_TRUE(ring.valid());
  EXPECT_FALSE(ring.begin_encode());
  for (std::size_t i = 0; i < 6; ++i) {
    const auto slot = ring.begin_conversion();
    ASSERT_EQ(slot, i % 2);
    EXPECT_FALSE(ring.begin_conversion());
    EXPECT_FALSE(ring.begin_encode());
    EXPECT_FALSE(ring.finish_encode(*slot));
    EXPECT_FALSE(ring.finish_conversion(2));
    EXPECT_FALSE(ring.finish_conversion(1 - *slot));
    ASSERT_TRUE(ring.finish_conversion(*slot));
    EXPECT_FALSE(ring.finish_conversion(*slot));
    ASSERT_EQ(ring.begin_encode(), slot);
    EXPECT_FALSE(ring.begin_encode());
    EXPECT_FALSE(ring.begin_conversion());
    EXPECT_FALSE(ring.finish_encode(2));
    EXPECT_FALSE(ring.finish_encode(1 - *slot));
    ASSERT_TRUE(ring.finish_encode(*slot));  // Caller has completed external fence wait.
    EXPECT_FALSE(ring.finish_encode(*slot));
    ASSERT_EQ(ring.begin_encode(), slot);  // Fresh encode of cached planes.
    ASSERT_TRUE(ring.finish_encode(*slot));
  }
}

TEST(PyrowavePlaneRingTest, DropsUnsubmittedReadyAndPrefersNewPlanesOverCache) {
  pyrowave::plane_ring_t ring;
  ASSERT_EQ(ring.begin_conversion(), 0u);
  ASSERT_TRUE(ring.finish_conversion(0));
  ASSERT_EQ(ring.begin_conversion(), 1u);
  ASSERT_TRUE(ring.finish_conversion(1));
  ASSERT_EQ(ring.begin_encode(), 1u);
  ASSERT_TRUE(ring.finish_encode(1));
  ASSERT_EQ(ring.begin_conversion(), 0u);
  ASSERT_TRUE(ring.finish_conversion(0));
  ASSERT_EQ(ring.begin_encode(), 0u);
  ASSERT_TRUE(ring.finish_encode(0));
  ASSERT_EQ(ring.begin_conversion(), 1u);
  ASSERT_TRUE(ring.finish_conversion(1));
  ASSERT_EQ(ring.begin_conversion(), 0u);  // Overwrites cached slot and drops ready 1.
  ASSERT_TRUE(ring.finish_conversion(0));
  ASSERT_EQ(ring.begin_encode(), 0u);
}

TEST(PyrowavePlaneRingTest, PoisoningPermanentlyRejectsAllReuse) {
  for (unsigned phase = 0; phase < 4; ++phase) {
    pyrowave::plane_ring_t ring;
    if (phase > 0) { ASSERT_EQ(ring.begin_conversion(), 0u); }
    if (phase > 1) { ASSERT_TRUE(ring.finish_conversion(0)); }
    if (phase > 2) { ASSERT_EQ(ring.begin_encode(), 0u); }
    ring.invalidate();
    ring.invalidate();
    EXPECT_FALSE(ring.valid());
    EXPECT_FALSE(ring.begin_conversion());
    EXPECT_FALSE(ring.begin_encode());
    EXPECT_FALSE(ring.finish_conversion(0));
    EXPECT_FALSE(ring.finish_encode(0));
  }
}

TEST(PyrowaveConfigTest, RejectsMissingApiVersion) {
  auto config = enabled_config();
  config.expected_api_version = {};
  EXPECT_FALSE(pyrowave::validate(config));
}

TEST(PyrowaveVideoAdmissionTest, RejectsSecondAndReacquiresAfterFailedStart) {
  pyrowave::video_admission_t gate;
  EXPECT_FALSE(gate.active());
  EXPECT_FALSE(gate.release());
  ASSERT_TRUE(gate.try_acquire());
  EXPECT_TRUE(gate.active());
  EXPECT_FALSE(gate.try_acquire());
  EXPECT_TRUE(gate.release());  // Failed-start guard releases the reservation.
  EXPECT_FALSE(gate.active());
  ASSERT_TRUE(gate.try_acquire());
  EXPECT_TRUE(gate.release());  // Normal join releases the next session.
  EXPECT_FALSE(gate.release());
}

TEST(PyrowaveVideoAdmissionTest, ReconfigurationRequiresActiveAndPreservesIt) {
  pyrowave::video_admission_t gate;
  EXPECT_FALSE(gate.try_begin_reconfiguration());
  ASSERT_TRUE(gate.try_acquire());
  ASSERT_TRUE(gate.try_begin_reconfiguration());
  EXPECT_FALSE(gate.try_begin_reconfiguration());
  EXPECT_FALSE(gate.try_acquire());
  gate.end_reconfiguration();
  EXPECT_TRUE(gate.active());
  EXPECT_FALSE(gate.try_acquire());
  EXPECT_TRUE(gate.release());
  EXPECT_TRUE(gate.try_acquire());
}

TEST(PyrowaveVideoAdmissionTest, ReleaseDuringReconfigurationKeepsAdmissionClosed) {
  pyrowave::video_admission_t gate;
  ASSERT_TRUE(gate.try_acquire());
  ASSERT_TRUE(gate.try_begin_reconfiguration());
  EXPECT_TRUE(gate.release());
  EXPECT_FALSE(gate.active());
  EXPECT_FALSE(gate.release());
  EXPECT_FALSE(gate.try_acquire());
  EXPECT_FALSE(gate.try_begin_reconfiguration());
  gate.end_reconfiguration();
  EXPECT_FALSE(gate.active());
  EXPECT_TRUE(gate.try_acquire());
}

TEST(PyrowaveVideoAdmissionTest, ConcurrentContendersHaveExactlyOneWinner) {
  pyrowave::video_admission_t gate;
  constexpr unsigned contenders = 16;
  std::atomic<bool> start {false};
  std::atomic<unsigned> attempts {0}, winners {0};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < contenders; ++i) {
    threads.emplace_back([&]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const bool acquired = gate.try_acquire();
      if (acquired) {
        winners.fetch_add(1);
      }
      attempts.fetch_add(1, std::memory_order_release);
      if (acquired) {
        while (attempts.load(std::memory_order_acquire) != contenders) {
          std::this_thread::yield();
        }
        gate.release();
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(attempts.load(), contenders);
  EXPECT_EQ(winners.load(), 1u);
  EXPECT_FALSE(gate.active());
  EXPECT_TRUE(gate.try_acquire());
}

TEST(PyrowaveSignalContractTest, AcceptsSdrBt709LimitedAndFullAtEvenResolutions) {
  static_assert(noexcept(pyrowave::validate_sdr_signal(2, 2, 0, 0, 2)));
  static_assert(pyrowave::validate_sdr_signal(2, 2, 0, 0, 2) == nullptr);
  for (const auto mode : { 2, 3 }) {
    EXPECT_EQ(pyrowave::validate_sdr_signal(2, 2, 0, 0, mode), nullptr);
    EXPECT_EQ(pyrowave::validate_sdr_signal(1920, 1080, 0, 0, mode), nullptr);
    EXPECT_EQ(pyrowave::validate_sdr_signal(1280, 720, 0, 0, mode), nullptr);
  }
}

TEST(PyrowaveSignalContractTest, RequiresFixedEqualCaptureSizeWithoutAlignment) {
  static_assert(noexcept(pyrowave::validate_capture_size(32, 30, 32, 30)));
  static_assert(pyrowave::validate_capture_size(32, 30, 32, 30) == nullptr);
  EXPECT_EQ(pyrowave::validate_capture_size(1920, 1080, 1920, 1080), nullptr);
  EXPECT_EQ(pyrowave::validate_capture_size(32, 30, 32, 30), nullptr);
  EXPECT_NE(pyrowave::validate_capture_size(1920, 1080, 1080, 1920), nullptr);
  EXPECT_NE(pyrowave::validate_capture_size(1920, 1080, 1920, 1200), nullptr);
  EXPECT_NE(pyrowave::validate_capture_size(1920, 1080, 1280, 720), nullptr);
  EXPECT_NE(pyrowave::validate_capture_size(1920, 1080, 0, 1080), nullptr);
  EXPECT_NE(pyrowave::validate_capture_size(1920, 1080, 1920, 0), nullptr);
  for (const auto dimension : { 0, -2, 31 }) {
    EXPECT_NE(pyrowave::validate_capture_size(dimension, 30, dimension, 30), nullptr);
    EXPECT_NE(pyrowave::validate_capture_size(32, dimension, 32, dimension), nullptr);
  }
}

TEST(PyrowaveSignalContractTest, RejectsNonpositiveOrOddDimensions) {
  for (const auto dimension : { 0, -1, -2, 1, 1919, std::numeric_limits<int>::min() }) {
    EXPECT_NE(pyrowave::validate_sdr_signal(dimension, 1080, 0, 0, 2), nullptr) << dimension;
    EXPECT_NE(pyrowave::validate_sdr_signal(1920, dimension, 0, 0, 3), nullptr) << dimension;
  }
}

TEST(PyrowaveSignalContractTest, RejectsHdrHlgAndUnknownDynamicRanges) {
  for (const auto range : { 1, 2, -1, 3, std::numeric_limits<int>::max() }) {
    for (const auto mode : { 2, 3, 4, 5 }) {
      EXPECT_NE(pyrowave::validate_sdr_signal(1920, 1080, range, 0, mode), nullptr) << range;
    }
  }
}

TEST(PyrowaveSignalContractTest, RejectsNon420Chroma) {
  for (const auto chroma : { -1, 1, 2, std::numeric_limits<int>::max() }) {
    EXPECT_NE(pyrowave::validate_sdr_signal(1920, 1080, 0, chroma, 2), nullptr) << chroma;
  }
}

TEST(PyrowaveSignalContractTest, RejectsRec601Bt2020AndMalformedCsc) {
  for (const auto mode : { 0, 1, 4, 5, -1, -2, 6, 7, 2 | 0x100, 3 | 0x100,
         std::numeric_limits<int>::min(), std::numeric_limits<int>::max() }) {
    const auto reason = pyrowave::validate_sdr_signal(1920, 1080, 0, 0, mode);
    ASSERT_NE(reason, nullptr) << mode;
    EXPECT_NE(*reason, '\0');
  }
}

TEST(PyrowaveConfigTest, RejectsOddYuv420Stream) {
  pyrowave::stream_config_t config;
  config.width = 1919;
  config.height = 1080;
  config.framerate_num = 60;
  EXPECT_FALSE(pyrowave::validate(config));
}

TEST(PyrowaveCapabilityTest, RequiresExactApiVersion) {
  auto config = enabled_config();
  auto device = supported_device();
  auto client = supported_client();
  client.api_version.minor = 5;

  const auto result = pyrowave::evaluate_capabilities(config, device, client);
  EXPECT_EQ(result.availability, pyrowave::availability_e::failed);
  EXPECT_EQ(result.failure, pyrowave::failure_e::api_mismatch);
}

TEST(PyrowavePacketTest, NegotiatedWireLimitAlsoBoundsOuterRtpGeometry) {
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1392, 65536), 1392);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1392, 1024), 1040);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1000, 1024), 1000);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(-1, 1024), 0);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1392, 128), 0);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1392, 65537), 0);
  EXPECT_TRUE(pyrowave::validate(pyrowave::packetization_request_t {
    .packet_boundary = LI_PYROWAVE_MAX_PACKET_SIZE,
  }));
  EXPECT_FALSE(pyrowave::validate(pyrowave::packetization_request_t {
    .packet_boundary = LI_PYROWAVE_MAX_PACKET_SIZE + 1u,
  }));
}

TEST(PyrowaveCapabilityTest, RequiresBlockAwareFec) {
  auto config = enabled_config();
  auto device = supported_device();
  auto client = supported_client();
  client.block_aware_fec = false;

  const auto result = pyrowave::evaluate_capabilities(config, device, client);
  EXPECT_EQ(result.availability, pyrowave::availability_e::failed);
  EXPECT_EQ(result.failure, pyrowave::failure_e::client_unsupported);
}

TEST(PyrowavePacketTest, OwnsEncodedBuffer) {
  auto bytes = std::make_shared<const std::vector<std::uint8_t>>(
    std::initializer_list<std::uint8_t> { 1, 2, 3 });
  pyrowave::encoded_frame_t frame;
  frame.frame_id = 1;
  frame.bitstream.storage = bytes;

  bytes.reset();
  ASSERT_TRUE(pyrowave::validate(frame));
  ASSERT_EQ(frame.bitstream.view().size(), 3u);
  EXPECT_EQ(frame.bitstream.view()[1], 2u);
}

TEST(PyrowavePacketTest, ValidatesPayloadPrefixAndDefaultsToWholeStorage) {
  pyrowave::encoded_frame_t frame {
    .frame_id = 1,
    .bitstream = { std::make_shared<const std::vector<std::uint8_t>>(301, 0x42) },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  ASSERT_EQ(frame.payload_size, 0u);
  ASSERT_TRUE(pyrowave::validate(frame));
  const auto whole = packetizer->packetize(frame, {});
  frame.payload_size = frame.bitstream.view().size();
  ASSERT_TRUE(pyrowave::validate(frame));
  const auto exact = packetizer->packetize(frame, {});
  ASSERT_EQ(whole.failure, pyrowave::failure_e::none);
  ASSERT_EQ(exact.failure, pyrowave::failure_e::none);
  EXPECT_EQ(*whole.bitstream.storage, *exact.bitstream.storage);
  ++frame.payload_size;
  EXPECT_FALSE(pyrowave::validate(frame));
  const auto rejected = packetizer->packetize(frame, {});
  EXPECT_NE(rejected.failure, pyrowave::failure_e::none);
  EXPECT_TRUE(rejected.bitstream.empty());
  EXPECT_TRUE(rejected.packets.empty());
}

TEST(PyrowavePacketTest, RejectsOutOfBoundsPacket) {
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(
    std::initializer_list<std::uint8_t> { 1, 2, 3 });
  pyrowave::packetization_result_t result;
  result.bitstream.storage = bytes;
  result.packets.push_back({ 2, 2 });

  EXPECT_FALSE(pyrowave::validate(result));
}

TEST(PyrowavePacketTest, TakesUniqueOutputAllocation) {
  pyrowave::packetization_result_t result;
  result.bitstream.storage = std::make_shared<std::vector<std::uint8_t>>(3, 0x5a);
  result.packets.push_back({ 0, 3 });
  const auto *data = result.bitstream.view().data();
  const auto bytes = result.take_bytes();
  EXPECT_EQ(bytes.data(), data);
  EXPECT_EQ(bytes, (std::vector<std::uint8_t> { 0x5a, 0x5a, 0x5a }));
  EXPECT_FALSE(result.bitstream.storage);
  EXPECT_TRUE(result.bitstream.empty());
  EXPECT_TRUE(result.packets.empty());
}

TEST(PyrowavePacketTest, CopiesAliasedOutputWithoutChangingOtherOwners) {
  pyrowave::packetization_result_t result;
  auto alias = std::make_shared<std::vector<std::uint8_t>>(3, 0x5a);
  result.bitstream.storage = alias;
  result.packets.push_back({ 0, 3 });
  auto bytes = result.take_bytes();
  EXPECT_NE(bytes.data(), alias->data());
  EXPECT_EQ(bytes, *alias);
  bytes[0] = 0;
  EXPECT_EQ(*alias, (std::vector<std::uint8_t> { 0x5a, 0x5a, 0x5a }));
  EXPECT_FALSE(result.bitstream.storage);
  EXPECT_TRUE(result.packets.empty());
}

TEST(PyrowavePacketTest, TakesNullOutputSafely) {
  pyrowave::packetization_result_t result;
  result.packets.push_back({ 0, 3 });
  EXPECT_TRUE(result.take_bytes().empty());
  EXPECT_FALSE(result.bitstream.storage);
  EXPECT_TRUE(result.packets.empty());
}

TEST(PyrowaveConfigTest, AcceptsStaticHlgTransfer) {
  pyrowave::config_t config = enabled_config();
  config.transfer = pyrowave::transfer_e::hlg;
  EXPECT_TRUE(pyrowave::validate(config));
}

TEST(PyrowaveConfigTest, AcceptsStaticHlgStream) {
  pyrowave::stream_config_t config;
  config.width = 1920;
  config.height = 1080;
  config.framerate_num = 60;
  config.transfer = pyrowave::transfer_e::hlg;
  EXPECT_TRUE(pyrowave::validate(config));
}

TEST(PyrowavePacketizerTest, WrapsFrameWithExactBoundedPackets) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(9000, 0x5a));
  pyrowave::encoded_frame_t frame {
    .frame_id = 7,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1024,
    .rtp_timestamp = 1234,
    .mark_critical = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  ASSERT_FALSE(result.packets.empty());
  ASSERT_TRUE(pyrowave::validate(result));

  for (std::size_t index = 0; index < result.packets.size(); ++index) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto span = result.packets[index];
    ASSERT_EQ(LiPyrowaveParsePacket(
                result.bitstream.view().data() + span.offset,
                span.size,
                &header,
                &payload), LI_PYROWAVE_PACKET_OK);
    EXPECT_EQ(header.frameId, 7u);
    EXPECT_EQ(header.rtpTimestamp, 1234u);
    if (header.packetKind == LI_PYROWAVE_PACKET_FRAME_HEADER) {
      EXPECT_EQ(index, 0u);
      EXPECT_EQ(header.blockIndex, 0u);
    }
    else {
      EXPECT_EQ(header.blockIndex, index - 1);
    }
    EXPECT_EQ(header.blockCount, result.packets.size() - 1);
    EXPECT_NE(header.flags & LI_PYROWAVE_FLAG_CRITICAL, 0u);
  }
}

TEST(PyrowavePacketizerTest, ProtectsMetadataAndStripsItBeforeDecode) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    std::initializer_list<std::uint8_t> { 0xa0, 0xb1, 0xc2, 0xd3 });
  pyrowave::encoded_frame_t frame {
    .frame_id = 13,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  const std::vector<std::uint8_t> metadata {
    0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x02, 0x00, 0x2a,
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1024,
    .rtp_timestamp = 1313,
    .metadata = metadata,
    .metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED |
                      LI_PYROWAVE_METADATA_FLAG_RUNTIME |
                      LI_PYROWAVE_METADATA_FLAG_OPTIONAL,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (const auto &span : result.packets) {
    const auto *packet = result.bitstream.view().data() + span.offset;
    const auto status = LiPyrowaveReassemblyPushPacket(&state, packet, span.size, 5000, 100000);
    EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                status == LI_PYROWAVE_REASSEMBLY_COMPLETE);
  }
  ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));

  std::vector<std::uint8_t> metadata_out(metadata.size());
  std::size_t metadata_length = 0;
  std::uint16_t metadata_flags = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyMetadata(
              &state, metadata_out.data(), metadata_out.size(), &metadata_length, &metadata_flags),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(metadata_length, metadata.size());
  EXPECT_EQ(metadata_out, metadata);
  EXPECT_EQ(metadata_flags, LI_PYROWAVE_METADATA_FLAG_PROTECTED |
                            LI_PYROWAVE_METADATA_FLAG_RUNTIME |
                            LI_PYROWAVE_METADATA_FLAG_OPTIONAL);

  std::vector<std::uint8_t> output(source->size());
  std::size_t output_length = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(
              &state, output.data(), output.size(), &output_length, nullptr),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(output_length, source->size());
  EXPECT_EQ(output, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, ReusesOnlyScratchAndPreservesEarlierResults) {
  auto packetizer = pyrowave::make_transport_packetizer();
  pyrowave::packetization_result_t retained, snapshot;
  for (unsigned i = 0; i < 24; ++i) {
    SCOPED_TRACE(i);
    const pyrowave::encoded_frame_t frame {
      .frame_id = i + 1,
      .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
      .bitstream = { std::make_shared<const std::vector<std::uint8_t>>(3001 + i * 37, i + 1) },
    };
    const bool metadata = (i & 4) != 0;
    const pyrowave::packetization_request_t request {
      .packet_boundary = (i & 1) ? LI_PYROWAVE_MAX_PACKET_SIZE : 128u,
      .rtp_timestamp = 9000 + i * 3000,
      .block_aware_fec = (i & 2) != 0,
      .metadata = metadata ? optional_metadata(137 + i) : std::vector<std::uint8_t> {},
      .metadata_flags = static_cast<std::uint16_t>(metadata ?
        LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL : 0),
    };
    if (i == 12) packetizer->reset();
    auto backing = std::make_shared<std::vector<std::uint8_t>>(*frame.bitstream.storage);
    backing->resize(backing->size() + 257, 0xa5);
    auto prefixed = frame;
    prefixed.bitstream.storage = backing;
    prefixed.payload_size = frame.bitstream.view().size();
    const auto reused = packetizer->packetize(prefixed, request);
    std::fill(backing->begin(), backing->end(), 0xcc);  // Next encode may overwrite all input.
    const auto fresh = pyrowave::make_transport_packetizer()->packetize(frame, request);
    ASSERT_EQ(reused.failure, pyrowave::failure_e::none);
    ASSERT_EQ(fresh.failure, pyrowave::failure_e::none);
    ASSERT_TRUE(pyrowave::validate(reused));
    ASSERT_TRUE(pyrowave::validate(fresh));
    EXPECT_NE(reused.bitstream.storage, fresh.bitstream.storage);
    EXPECT_EQ(*reused.bitstream.storage, *fresh.bitstream.storage);
    ASSERT_EQ(reused.packets.size(), fresh.packets.size());
    for (std::size_t j = 0; j < reused.packets.size(); ++j) {
      EXPECT_EQ(reused.packets[j].offset, fresh.packets[j].offset);
      EXPECT_EQ(reused.packets[j].size, fresh.packets[j].size);
    }
    if (i == 0) { retained = reused; snapshot = fresh; }
    EXPECT_EQ(*retained.bitstream.storage, *snapshot.bitstream.storage);
    ASSERT_EQ(retained.packets.size(), snapshot.packets.size());
    for (std::size_t j = 0; j < retained.packets.size(); ++j) {
      EXPECT_EQ(retained.packets[j].offset, snapshot.packets[j].offset);
      EXPECT_EQ(retained.packets[j].size, snapshot.packets[j].size);
    }
  }
}

TEST(PyrowavePacketizerTest, RejectsMetadataLargerThanProtocolLimit) {
  const pyrowave::encoded_frame_t frame {
    .frame_id = 14,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { std::make_shared<const std::vector<std::uint8_t>>(4, 0x5a) },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  for (const std::size_t boundary : { 128u, 0u, LI_PYROWAVE_MAX_PACKET_SIZE }) {
    for (const bool fec : { false, true }) {
      SCOPED_TRACE(::testing::Message() << "boundary=" << boundary << ", fec=" << fec);
      const pyrowave::packetization_request_t request {
        .packet_boundary = boundary,
        .block_aware_fec = fec,
        .metadata = optional_metadata(LI_PYROWAVE_MAX_METADATA_SIZE + 1),
        .metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL,
      };
      // Fail before exercising the copy if the request guard regresses.
      ASSERT_FALSE(pyrowave::validate(request));
      const auto result = packetizer->packetize(frame, request);
      EXPECT_EQ(result.failure, pyrowave::failure_e::configuration_invalid);
      EXPECT_TRUE(result.bitstream.empty());
      EXPECT_TRUE(result.packets.empty());
    }
  }
}

TEST(PyrowavePacketizerTest, RoundTripsProtectedMetadataAcrossPacketsAndLostBlock) {
  const auto source = std::make_shared<const std::vector<std::uint8_t>>(4, 0xa5);
  const pyrowave::encoded_frame_t frame {
    .frame_id = 15,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  for (const std::size_t boundary : { 128u, 0u, LI_PYROWAVE_MAX_PACKET_SIZE }) {
    const auto effective_boundary = boundary == 0 ? LI_PYROWAVE_MAX_PACKET_SIZE : boundary;
    for (const bool fec : { false, true }) {
      SCOPED_TRACE(::testing::Message() << "boundary=" << boundary << ", fec=" << fec);
      const pyrowave::packetization_request_t request {
        .packet_boundary = boundary,
        .block_aware_fec = fec,
        .metadata = optional_metadata(std::min<std::size_t>(effective_boundary * 3, LI_PYROWAVE_MAX_METADATA_SIZE)),
        .metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL,
      };
      ASSERT_TRUE(pyrowave::validate(request));
      const auto result = packetizer->packetize(frame, request);
      ASSERT_EQ(result.failure, pyrowave::failure_e::none);
      LI_PYROWAVE_REASSEMBLY_STATE state;
      LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
      bool dropped_data = false;
      for (const auto &packet : result.packets) {
        EXPECT_LE(packet.size, effective_boundary);
        LI_PYROWAVE_PACKET_HEADER header {};
        const std::uint8_t *payload = nullptr;
        ASSERT_EQ(LiPyrowaveParsePacket(result.bitstream.view().data() + packet.offset, packet.size, &header, &payload),
                  LI_PYROWAVE_PACKET_OK);
        if (fec && !dropped_data && header.packetKind == LI_PYROWAVE_PACKET_DATA &&
            (header.flags & LI_PYROWAVE_FLAG_FEC_PARITY) == 0) {
          dropped_data = true;
          continue;
        }
        const auto status = LiPyrowaveReassemblyPushPacket(&state,
          result.bitstream.view().data() + packet.offset, packet.size, 5000, 100000);
        EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                    status == LI_PYROWAVE_REASSEMBLY_COMPLETE ||
                    status == LI_PYROWAVE_REASSEMBLY_DUPLICATE);
      }
      EXPECT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
      std::vector<std::uint8_t> metadata_out(request.metadata.size());
      std::size_t written = 0;
      EXPECT_EQ(LiPyrowaveReassemblyCopyMetadata(&state, metadata_out.data(), metadata_out.size(),
        &written, nullptr), LI_PYROWAVE_REASSEMBLY_COMPLETE);
      EXPECT_EQ(written, request.metadata.size());
      EXPECT_EQ(metadata_out, request.metadata);
      std::vector<std::uint8_t> decoded_bytes(source->size());
      EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(&state, decoded_bytes.data(), decoded_bytes.size(),
        &written, nullptr), LI_PYROWAVE_REASSEMBLY_COMPLETE);
      EXPECT_EQ(written, source->size());
      EXPECT_EQ(decoded_bytes, *source);
      LiPyrowaveReassemblyDestroy(&state);
    }
  }
}

TEST(PyrowavePacketizerTest, PreservesFrameHeaderAndTailPaddingOnTheWire) {
  // Reassembly ignores the repeated frame-header payload and trimmed tail bytes.
  // Fixed wire examples protect those bytes without another packetizer algorithm.
  const std::vector<std::uint8_t> metadata { 0x80, 0, 0, 5, 0, 0, 0, 0 };
  const pyrowave::encoded_frame_t frame {
    .frame_id = 17,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { std::make_shared<const std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t> {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20 }) },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  for (const bool fec : { false, true }) {
    SCOPED_TRACE(fec);
    const auto result = packetizer->packetize(frame, {
      .packet_boundary = 80,
      .rtp_timestamp = 1717,
      .block_aware_fec = fec,
      .mark_critical = true,
      .metadata = metadata,
      .metadata_flags = 5,
    });
    ASSERT_EQ(result.failure, pyrowave::failure_e::none);
    ASSERT_TRUE(pyrowave::validate(result));
    std::vector<std::vector<std::uint8_t>> expected {
      { 0x80, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
      { 0x80, 0, 0, 5, 0, 0, 0, 0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 },
      { 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20 },
    };
    if (fec) {
      expected[2].resize(16, 0);
      expected.push_back({ 0x98, 0x19, 0x1a, 0x1e, 0x1c, 0x1d, 0x1e, 0x1f,
                           0x30, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 });
    }
    const unsigned kinds[] { 0, 1, 1, 2 };
    const unsigned flags[] { 0x15, 0x15, 0x16, 0x1c };
    ASSERT_EQ(result.packets.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      SCOPED_TRACE(i);
      const auto span = result.packets[i];
      EXPECT_EQ(span.offset, i * 80);
      EXPECT_EQ(span.size, 64 + expected[i].size());
      LI_PYROWAVE_PACKET_HEADER header {};
      const std::uint8_t *payload = nullptr;
      ASSERT_EQ(LiPyrowaveParsePacket(result.bitstream.view().data() + span.offset, span.size,
        &header, &payload), LI_PYROWAVE_PACKET_OK);
      EXPECT_EQ(header.packetKind, kinds[i]);
      EXPECT_EQ(header.flags, flags[i]);
      EXPECT_EQ(header.codecPayloadLength, 17u);
      EXPECT_EQ(header.protectedPayloadLength, 25u);
      EXPECT_EQ(header.metadataLength, 8u);
      EXPECT_EQ(header.dataBlockCount, 2u);
      EXPECT_EQ(header.parityBlockCount, fec ? 1u : 0u);
      EXPECT_EQ(header.blockCount, fec ? 3u : 2u);
      EXPECT_EQ(std::vector<std::uint8_t>(payload, payload + header.payloadLength), expected[i]);
    }
  }
}

TEST(PyrowavePacketizerTest, ClientRecoversMetadataAndCodecAcrossEverySegmentBoundary) {
  constexpr std::size_t payload_size = 32;
  constexpr std::size_t boundary = LI_PYROWAVE_WIRE_HEADER_SIZE + payload_size;
  auto packetizer = pyrowave::make_transport_packetizer();
  for (const auto metadata_size : { 0u, 8u, 31u, 32u, 33u, 63u, 64u, 65u }) {
    for (const auto codec_size : { 1u, 31u, 32u, 33u, 577u }) {
      for (const bool fec : { false, true }) {
        SCOPED_TRACE(::testing::Message() << "metadata=" << metadata_size << ", codec=" << codec_size << ", fec=" << fec);
        auto codec = std::make_shared<std::vector<std::uint8_t>>(codec_size);
        for (std::size_t i = 0; i < codec->size(); ++i) {
          (*codec)[i] = static_cast<std::uint8_t>(i * 37 + 11);
        }
        const pyrowave::encoded_frame_t frame {
          .frame_id = 17,
          .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
          .bitstream = { codec },
        };
        const pyrowave::packetization_request_t request {
          .packet_boundary = boundary,
          .rtp_timestamp = 1717,
          .block_aware_fec = fec,
          .mark_critical = true,
          .metadata = metadata_size == 0 ? std::vector<std::uint8_t> {} : optional_metadata(metadata_size),
          .metadata_flags = static_cast<std::uint16_t>(metadata_size == 0 ? 0 :
            LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL),
        };
        const auto result = packetizer->packetize(frame, request);
        ASSERT_EQ(result.failure, pyrowave::failure_e::none);
        ASSERT_TRUE(pyrowave::validate(result));

        // The independently implemented client reassembler is the oracle;
        // do not maintain a second copy of the host's packet/FEC construction.
        LI_PYROWAVE_REASSEMBLY_STATE state;
        LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
        const auto cleanup = std::unique_ptr<LI_PYROWAVE_REASSEMBLY_STATE,
          decltype(&LiPyrowaveReassemblyDestroy)>(&state, LiPyrowaveReassemblyDestroy);
        bool dropped_data = false;
        for (const auto &span : result.packets) {
          EXPECT_LE(span.size, boundary);
          const auto *wire = result.bitstream.view().data() + span.offset;
          LI_PYROWAVE_PACKET_HEADER header {};
          const std::uint8_t *payload = nullptr;
          ASSERT_EQ(LiPyrowaveParsePacket(wire, span.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
          EXPECT_EQ(header.frameId, 17u);
          EXPECT_EQ(header.rtpTimestamp, 1717u);
          EXPECT_NE(header.flags & LI_PYROWAVE_FLAG_CRITICAL, 0);
          if (fec && !dropped_data && header.packetKind == LI_PYROWAVE_PACKET_DATA) {
            dropped_data = true;
            continue;
          }
          const auto status = LiPyrowaveReassemblyPushPacket(&state, wire, span.size, 5000, 100000);
          EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                      status == LI_PYROWAVE_REASSEMBLY_COMPLETE ||
                      status == LI_PYROWAVE_REASSEMBLY_DUPLICATE);
        }
        ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
        std::vector<std::uint8_t> decoded_metadata(metadata_size);
        std::vector<std::uint8_t> decoded_codec(codec_size);
        std::size_t written = 0;
        std::uint16_t flags = 0;
        ASSERT_EQ(LiPyrowaveReassemblyCopyMetadata(&state, decoded_metadata.data(), decoded_metadata.size(),
          &written, &flags), LI_PYROWAVE_REASSEMBLY_COMPLETE);
        EXPECT_EQ(written, metadata_size);
        EXPECT_EQ(flags, request.metadata_flags);
        EXPECT_EQ(decoded_metadata, request.metadata);
        ASSERT_EQ(LiPyrowaveReassemblyCopyFrame(&state, decoded_codec.data(), decoded_codec.size(),
          &written, nullptr), LI_PYROWAVE_REASSEMBLY_COMPLETE);
        EXPECT_EQ(written, codec_size);
        EXPECT_EQ(decoded_codec, *codec);
      }
    }
  }
}

#ifdef _WIN32
TEST(PyrowaveDynamicHdrTest, ProducesEachDesktopMetadataProfile) {
  const auto source = hdr_source_metadata();
  const auto stats = hdr_scene_stats();
  for (int format = 1; format <= 5; ++format) {
    pyrowave::hdr_metadata_producer_t producer;
    // The shared fixture payloads use the source's 1000-nit target. Target
    // dependent Vivid behavior is covered separately; keep this test focused
    // on the registered type and the golden syntax vectors.
    ASSERT_TRUE(producer.configure(format, source, 1000));
    EXPECT_TRUE(producer.enabled());
    const auto *metadata = producer.build(stats);
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->type, LiPyrowaveDynamicHdrMetadataType(format));
    EXPECT_EQ(metadata->hlg_nominal_peak_nits, (format == 3 || format == 5) ? 1000 : 0);
    ASSERT_FALSE(metadata->payload.empty());
    const auto &fixture = PyrowaveDynamicHdrFixtures[format - 1];
    const std::string_view hex(fixture.payloadHex);
    std::vector<std::uint8_t> expected;
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      expected.push_back(static_cast<std::uint8_t>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    }
    // This shared fixture targets the same 1000-nit peak as its source scene,
    // so HDR10+ does not need a tone-mapping curve here. The fixture's two
    // targeted-display bytes retain its historical 500-nit value; verify the
    // target-independent syntax around that field and cover target-dependent
    // generation in the dedicated metadata tests.
    if (format == 1) {
      ASSERT_EQ(metadata->payload.size(), expected.size());
      EXPECT_TRUE(std::equal(metadata->payload.begin(), metadata->payload.begin() + 9, expected.begin()));
      EXPECT_TRUE(std::equal(metadata->payload.begin() + 11, metadata->payload.end(), expected.begin() + 11));
    }
    else {
      EXPECT_EQ(metadata->payload, expected);
    }
    ASSERT_TRUE(producer.configure(0, source, 500));
    EXPECT_FALSE(producer.enabled());
    EXPECT_EQ(producer.build(stats), nullptr);
  }
}

TEST(PyrowaveDynamicHdrTest, DoesNotInventMetadataBeforeTheFirstValidScene) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    EXPECT_EQ(producer.build({}), nullptr);
    auto invalid = hdr_scene_stats();
    invalid.avg_maxrgb = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(producer.build(invalid), nullptr);
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
  }
}

TEST(PyrowaveDynamicHdrTest, KeepsValidSceneMetadataDuringReadbackGaps) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    // The first Dolby RPU has scene_refresh; subsequent pictures must not.
    const auto *metadata = producer.build(hdr_scene_stats());
    ASSERT_NE(metadata, nullptr);
    const auto expected = *metadata;
    for (int frame = 0; frame < 8; ++frame) {
      metadata = producer.build({});
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->type, expected.type);
      EXPECT_EQ(metadata->hlg_nominal_peak_nits, expected.hlg_nominal_peak_nits);
      EXPECT_EQ(metadata->payload, expected.payload);
    }
  }
}

TEST(PyrowaveDynamicHdrTest, InvalidNewSamplesDoNotPoisonTheCachedScene) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    const auto stats = hdr_scene_stats();
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    ASSERT_NE(producer.build(stats), nullptr);
    const auto *metadata = producer.build(stats);
    ASSERT_NE(metadata, nullptr);
    const auto expected = metadata->payload;

    std::vector<platf::hdr_frame_luminance_stats_t> invalid(7, stats);
    invalid[0].avg_maxrgb_pq = std::numeric_limits<float>::quiet_NaN();
    invalid[1].avg_maxrgb = std::numeric_limits<float>::infinity();
    invalid[2].min_maxrgb = -1;
    invalid[3].percentile_99 = -1;
    invalid[4].percentile_90_pq = 1.1f;
    invalid[5].distribution_maxrgb[3] = std::numeric_limits<float>::quiet_NaN();
    invalid[6].percentile_10_pq = -0.1f;
    if (format != 1) {
      auto missing_pq_average = stats;
      missing_pq_average.avg_maxrgb_pq = 0;
      invalid.push_back(missing_pq_average);
    }
    for (auto &candidate : invalid) {
      candidate.sample_sequence = 2;
      metadata = producer.build(candidate);
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->payload, expected);
      metadata = producer.build(stats);
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->payload, expected);
    }
  }
}

TEST(PyrowaveDynamicHdrTest, ANewSceneReplacesCachedMetadata) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    const auto *metadata = producer.build(hdr_scene_stats());
    ASSERT_NE(metadata, nullptr);
    const auto previous = metadata->payload;

    auto scene = hdr_scene_stats();
    scene.sample_sequence = 2;
    scene.max_maxrgb = 3000;
    scene.avg_maxrgb = 1000;
    scene.avg_maxrgb_pq = video::hdr_metadata::nits_to_pq(1000);
    scene.percentile_90_pq = video::hdr_metadata::nits_to_pq(2100);
    scene.percentile_99 = 3000;
    scene.analysis_max_nits = 3000;
    for (auto &value : scene.distribution_maxrgb) value *= 3;
    metadata = producer.build(scene);
    ASSERT_NE(metadata, nullptr);
    EXPECT_NE(metadata->payload, previous);
    metadata = producer.build({});
    ASSERT_NE(metadata, nullptr);
    const auto current = metadata->payload;
    EXPECT_NE(current, previous);
    metadata = producer.build({});
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->payload, current);
  }
}

TEST(PyrowaveDynamicHdrTest, ReconfigurationClearsThePreviousScene) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    const auto source = hdr_source_metadata();
    ASSERT_TRUE(producer.configure(format, source, 500));
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    ASSERT_TRUE(producer.configure(format, source, 600));
    EXPECT_EQ(producer.build({}), nullptr);
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    ASSERT_TRUE(producer.configure(format % 5 + 1, source, 600));
    EXPECT_EQ(producer.build({}), nullptr);
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    ASSERT_TRUE(producer.configure(0, source, 600));
    EXPECT_EQ(producer.build(hdr_scene_stats()), nullptr);
    ASSERT_TRUE(producer.configure(format, source, 600));
    EXPECT_EQ(producer.build({}), nullptr);
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    EXPECT_FALSE(producer.configure(99, source, 600));
    EXPECT_FALSE(producer.enabled());
    EXPECT_EQ(producer.build({}), nullptr);
  }
}

TEST(PyrowaveDynamicHdrTest, ABlackSceneCanInitializeMetadata) {
  platf::hdr_frame_luminance_stats_t black {};
  black.valid = true;
  black.sample_sequence = 1;
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    const auto *metadata = producer.build(black);
    ASSERT_NE(metadata, nullptr);
    EXPECT_FALSE(metadata->payload.empty());
    EXPECT_NE(producer.build({}), nullptr);
  }
}

TEST(PyrowaveDynamicHdrTest, DolbySceneRefreshIsNotRepeatedAcrossReadbackGaps) {
  const auto source = hdr_source_metadata();
  const auto stats = hdr_scene_stats();
  for (int format : { 4, 5 }) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, source, 500));
    video::dolby_vision::rpu_generator_t reference;
    ASSERT_TRUE(reference.configure({
      .source_mastering_peak_nits = source.maxDisplayLuminance,
      .mastering_min_nits_x10000 = source.minDisplayLuminance,
      .max_cll_nits = source.maxContentLightLevel,
      .max_fall_nits = source.maxFrameAverageLightLevel,
    }));
    auto raw = video::dolby_vision::frame_metadata_from_stats(stats);
    ASSERT_TRUE(raw);
    raw->scene_refresh = true;
    auto expected = reference.generate(*raw);
    const auto *metadata = producer.build(stats);
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->payload, (std::vector<std::uint8_t>(expected.begin(), expected.end())));
    raw->scene_refresh = false;
    expected = reference.generate(*raw);
    for (int frame = 0; frame < 3; ++frame) {
      metadata = producer.build({});
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->payload, (std::vector<std::uint8_t>(expected.begin(), expected.end())));
    }
  }
}

TEST(PyrowaveDynamicHdrTest, DolbySmoothingAdvancesPerEncodedFrameWithoutANewSample) {
  const auto source = hdr_source_metadata();
  const auto initial = hdr_scene_stats();
  auto next = initial;
  next.sample_sequence = 2;
  next.avg_maxrgb = 115;
  next.avg_maxrgb_pq = video::hdr_metadata::nits_to_pq(115);
  next.percentile_99 = 1100;
  for (int format : { 4, 5 }) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, source, 500));
    ASSERT_NE(producer.build(initial), nullptr);
    video::dolby_vision::rpu_generator_t reference;
    ASSERT_TRUE(reference.configure({
      .source_mastering_peak_nits = source.maxDisplayLuminance,
      .mastering_min_nits_x10000 = source.minDisplayLuminance,
      .max_cll_nits = source.maxContentLightLevel,
      .max_fall_nits = source.maxFrameAverageLightLevel,
    }));
    const auto initial_raw = video::dolby_vision::frame_metadata_from_stats(initial);
    const auto next_raw = video::dolby_vision::frame_metadata_from_stats(next);
    ASSERT_TRUE(initial_raw);
    ASSERT_TRUE(next_raw);
    video::dolby_vision::level1_temporal_filter_t filter;
    filter.update(*initial_raw);
    for (int frame = 0; frame < 3; ++frame) {
      const auto expected = reference.generate(filter.update(*next_raw));
      const auto *metadata = producer.build(frame == 0 ? next : platf::hdr_frame_luminance_stats_t {});
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->payload, (std::vector<std::uint8_t>(expected.begin(), expected.end())));
    }
  }
}

TEST(PyrowaveCapabilityTest, RejectsAnUpstreamRuntimeInTheForkContract) {
  auto config = enabled_config();
  auto device = supported_device();
  auto client = supported_client();
  device.api_version = { 1, 0, 0 };
  auto result = pyrowave::evaluate_capabilities(config, device, client);
  EXPECT_EQ(result.failure, pyrowave::failure_e::api_mismatch);
  device = supported_device();
  client.api_version = { 1, 0, 0 };
  result = pyrowave::evaluate_capabilities(config, device, client);
  EXPECT_EQ(result.failure, pyrowave::failure_e::api_mismatch);
}

TEST(PyrowaveTransportTest, RecoversRuntimeMetadataAfterLossAndReordering) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets = mail->queue<video::packet_t>("pyrowave-test-runtime");
  const auto source = std::make_shared<const std::vector<std::uint8_t>>(6000, 0x63);
  const auto published = platf::pyrowave_windows::publish_transport_frame(
    1, source, 128, 10, packets, nullptr, std::chrono::steady_clock::now(), std::nullopt);
  ASSERT_TRUE(published.success);
  auto output = packets->pop();
  ASSERT_NE(output, nullptr);
  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  // Reverse arrival, no FRAME_HEADER, and a missing first DATA block. Runtime
  // metadata must be recovered from DATA/PARITY, not its header copy.
  for (std::size_t i = published.blocks; i-- > 0;) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto *packet = output->data() + i * 192;
    ASSERT_EQ(LiPyrowaveParsePacket(packet, 192, &header, &payload), LI_PYROWAVE_PACKET_OK);
    if (header.packetKind == LI_PYROWAVE_PACKET_FRAME_HEADER ||
        (header.packetKind == LI_PYROWAVE_PACKET_DATA && header.blockIndex == 0)) continue;
    LiPyrowaveReassemblyPushPacket(&state, packet, 192, 5000, 100000);
  }
  ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  std::vector<std::uint8_t> recovered(10);
  std::size_t written = 0;
  std::uint16_t flags = 0;
  ASSERT_EQ(LiPyrowaveReassemblyCopyMetadata(&state, recovered.data(), recovered.size(), &written, &flags),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(written, 10u);
  constexpr auto expected_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED |
    LI_PYROWAVE_METADATA_FLAG_RUNTIME | LI_PYROWAVE_METADATA_FLAG_OPTIONAL;
  EXPECT_EQ(flags, expected_flags);
  EXPECT_EQ((recovered[0] << 8) | recovered[1], LI_PYROWAVE_METADATA_HOST_PROCESSING_LATENCY);
  EXPECT_EQ((recovered[2] << 8) | recovered[3], expected_flags);
  EXPECT_EQ((std::vector<std::uint8_t>(recovered.begin() + 4, recovered.begin() + 8)),
            (std::vector<std::uint8_t> { 0, 0, 0, 2 }));
  std::vector<std::uint8_t> decoded(source->size());
  ASSERT_EQ(LiPyrowaveReassemblyCopyFrame(&state, decoded.data(), decoded.size(), &written, nullptr),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(written, source->size());
  EXPECT_EQ(decoded, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowaveTransportTest, PublishesFrameWithProtectedRuntimeMetadata) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets = mail->queue<video::packet_t>("pyrowave-test-video");
  auto source = std::make_shared<const std::vector<std::uint8_t>>(323933, 0x5a);
  const auto timestamp = std::chrono::steady_clock::now() - std::chrono::milliseconds(12);
  const auto result = platf::pyrowave_windows::publish_transport_frame(
    1, source, 1312, 0, packets, nullptr, timestamp, std::nullopt);

  ASSERT_TRUE(result.success);
  ASSERT_TRUE(packets->peek());
  auto packet = packets->pop(std::chrono::milliseconds(0));
  ASSERT_TRUE(packet);
  const auto &output = packet;
  ASSERT_EQ(output->data_size(), result.bytes);
  ASSERT_GT(result.blocks, 255u);
  LI_PYROWAVE_PACKET_HEADER header {};
  const std::uint8_t *payload = nullptr;
  ASSERT_EQ(LiPyrowaveParsePacket(output->data(), 1376, &header, &payload), LI_PYROWAVE_PACKET_OK);
  EXPECT_EQ(header.metadataLength, 10u);
  EXPECT_EQ(header.metadataFlags, LI_PYROWAVE_METADATA_FLAG_PROTECTED |
                                 LI_PYROWAVE_METADATA_FLAG_RUNTIME |
                                 LI_PYROWAVE_METADATA_FLAG_OPTIONAL);

  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (std::size_t offset = 0; offset < output->data_size(); offset += 1376) {
    const auto status = LiPyrowaveReassemblyPushPacket(&state,
      output->data() + offset, 1376, 1000, 100000);
    EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                status == LI_PYROWAVE_REASSEMBLY_COMPLETE);
  }
  ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  std::vector<std::uint8_t> metadata(10);
  std::size_t metadata_length = 0;
  std::uint16_t metadata_flags = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyMetadata(&state, metadata.data(), metadata.size(),
              &metadata_length, &metadata_flags), LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(metadata_length, 10u);
  EXPECT_EQ((static_cast<std::uint16_t>(metadata[0]) << 8) | metadata[1],
            LI_PYROWAVE_METADATA_HOST_PROCESSING_LATENCY);
  EXPECT_EQ((static_cast<std::uint16_t>(metadata[2]) << 8) | metadata[3], metadata_flags);
  std::vector<std::uint8_t> decoded(source->size());
  std::size_t decoded_length = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(&state, decoded.data(), decoded.size(),
              &decoded_length, nullptr), LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(decoded_length, source->size());
  EXPECT_EQ(decoded, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowaveTransportTest, PublishesReplayWithoutMetadataFlags) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets = mail->queue<video::packet_t>("pyrowave-test-video");
  auto source = std::make_shared<const std::vector<std::uint8_t>>(9000, 0x3c);
  const auto result = platf::pyrowave_windows::publish_transport_frame(
    2, source, 1312, 1500, packets, nullptr, std::nullopt, std::nullopt);

  ASSERT_TRUE(result.success);
  auto packet = packets->pop(std::chrono::milliseconds(0));
  ASSERT_TRUE(packet);
  LI_PYROWAVE_PACKET_HEADER header {};
  const std::uint8_t *payload = nullptr;
  ASSERT_EQ(LiPyrowaveParsePacket(packet->data(), 1376, &header, &payload), LI_PYROWAVE_PACKET_OK);
  EXPECT_EQ(header.metadataLength, 0u);
  EXPECT_EQ(header.metadataFlags, 0u);
  EXPECT_EQ(header.flags & LI_PYROWAVE_FLAG_METADATA_PRESENT, 0u);
}

TEST(PyrowaveTransportTest, DoesNotQueueFailedPacketization) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets = mail->queue<video::packet_t>("pyrowave-test-video");
  auto source = std::make_shared<const std::vector<std::uint8_t>>(9000, 0x4d);
  // This positive host frame ID cannot be represented by the wire header and
  // makes the real packetizer return a valid, payload-free failure result.
  const auto frame_id = static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  const auto result = platf::pyrowave_windows::publish_transport_frame(
    frame_id, source, 1312, 0, packets, nullptr, std::nullopt, std::nullopt);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.result_code, 0);
  EXPECT_EQ(result.bytes, 0u);
  EXPECT_EQ(result.blocks, 0u);
  EXPECT_FALSE(packets->peek());
}

TEST(PyrowaveRateControlTest, KeepsLowBitrateBudgetBelowLegacyFloor) {
  using platf::pyrowave_windows::pyrowave_frame_budget;

  EXPECT_EQ(pyrowave_frame_budget(10'000, 120, 1), 10'416u);
  EXPECT_EQ(pyrowave_frame_budget(100'000, 120, 1), 104'166u);
  EXPECT_EQ(pyrowave_frame_budget(1'000, 120, 1), 1'041u);
  EXPECT_EQ(pyrowave_frame_budget(100, 120, 1), 104u);
}

TEST(PyrowaveRateControlTest, UsesFixedNegotiatedFrameRate) {
  using platf::pyrowave_windows::pyrowave_frame_budget;

  EXPECT_EQ(pyrowave_frame_budget(20'000, 120, 1), 20'833u);
  EXPECT_EQ(pyrowave_frame_budget(20'000, 60, 1), 41'666u);
  EXPECT_EQ(pyrowave_frame_budget(600'000, 60, 1), 1u * 1024u * 1024u);
}

TEST(PyrowaveRateControlTest, DoesNotDoubleBitrateAt120Fps) {
  constexpr int bitrate_kbps = 104'300;
  constexpr int frame_rate = 120;
  const auto budget = platf::pyrowave_windows::pyrowave_frame_budget(bitrate_kbps, frame_rate, 1);

  EXPECT_EQ(budget, 108'645u);
  EXPECT_LE(static_cast<std::uint64_t>(budget) * 8u * frame_rate,
            static_cast<std::uint64_t>(bitrate_kbps) * 1000u);
}

TEST(PyrowaveRateControlTest, PreservesFractionalNegotiatedFrameRate) {
  using platf::pyrowave_windows::pyrowave_frame_budget;

  EXPECT_EQ(pyrowave_frame_budget(60'000, 60'000, 1001), 125'125u);
  EXPECT_EQ(pyrowave_frame_budget(60'000, 120'000, 1001), 62'562u);
}

TEST(PyrowaveColorMetadataTest, PreservesLimitedAndFullRangeForAllSupportedModes) {
  for (const int dynamic_range : { 0, 1, 2 }) {
    const auto limited = platf::pyrowave_windows::color_metadata_for(dynamic_range, false);
    const auto full = platf::pyrowave_windows::color_metadata_for(dynamic_range, true);

    EXPECT_EQ(limited.range, PYROWAVE_YCBCR_LIMITED);
    EXPECT_EQ(full.range, PYROWAVE_YCBCR_FULL);
    EXPECT_EQ(limited.primaries, dynamic_range == 0
      ? PYROWAVE_COLOR_PRIMARIES_BT709 : PYROWAVE_COLOR_PRIMARIES_BT2020);
    EXPECT_EQ(limited.transfer, dynamic_range == 0
      ? PYROWAVE_TRANSFER_BT709
      : dynamic_range == 1 ? PYROWAVE_TRANSFER_PQ : PYROWAVE_TRANSFER_HLG);
  }
}

TEST(PyrowaveColorMetadataTest, UsesResolvedConversionColorSpace) {
  using video::colorspace_e;
  using platf::pyrowave_windows::color_metadata_for;
  const auto sdr = color_metadata_for({ colorspace_e::rec709, false, 8 });
  ASSERT_TRUE(sdr);
  EXPECT_EQ(sdr->transfer, PYROWAVE_TRANSFER_BT709);
  EXPECT_EQ(sdr->range, PYROWAVE_YCBCR_LIMITED);
  const auto pq = color_metadata_for({ colorspace_e::bt2020, true, 10 });
  ASSERT_TRUE(pq);
  EXPECT_EQ(pq->transfer, PYROWAVE_TRANSFER_PQ);
  EXPECT_EQ(pq->range, PYROWAVE_YCBCR_FULL);
  const auto hlg = color_metadata_for({ colorspace_e::bt2020hlg, false, 10 });
  ASSERT_TRUE(hlg);
  EXPECT_EQ(hlg->transfer, PYROWAVE_TRANSFER_HLG);
  EXPECT_FALSE(color_metadata_for({ colorspace_e::bt2020sdr, true, 10 }));
  EXPECT_FALSE(color_metadata_for({ colorspace_e::rec709, true, 10 }));
  EXPECT_FALSE(color_metadata_for({ colorspace_e::bt2020, true, 8 }));
}
#endif

TEST(PyrowavePacketizerTest, HonorsSmallerNegotiatedWireLimitWithoutSplittingInnerPackets) {
  const auto rtp_size = pyrowave::limit_rtp_packet_size(1392, 1024);
  ASSERT_EQ(rtp_size, 1040);
  const auto outer_payload_size = static_cast<std::size_t>(rtp_size - 16);
  pyrowave::encoded_frame_t frame {
    .frame_id = 1,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { std::make_shared<const std::vector<std::uint8_t>>(9000, 0x5a) },
  };
  const auto result = pyrowave::make_transport_packetizer()->packetize(frame, {
    .packet_boundary = outer_payload_size,
    .block_aware_fec = true,
  });
  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  ASSERT_FALSE(result.packets.empty());
  for (const auto &packet : result.packets) {
    EXPECT_EQ(packet.size, outer_payload_size);
    EXPECT_EQ(packet.offset % outer_payload_size, 0u);
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    EXPECT_EQ(LiPyrowaveParsePacket(result.bitstream.view().data() + packet.offset,
      packet.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
  }
}

TEST(PyrowavePacketizerTest, RecoversOneMissingBlockWithBlockAwareFec) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(5000, 0x5a));
  pyrowave::encoded_frame_t frame {
    .frame_id = 8,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1240,
    .rtp_timestamp = 5678,
    .block_aware_fec = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  ASSERT_GT(result.packets.size(), 2u);

  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (std::size_t index = 0; index < result.packets.size(); ++index) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto span = result.packets[index];
    ASSERT_EQ(LiPyrowaveParsePacket(result.bitstream.view().data() + span.offset,
                                    span.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
    if (index == 1) continue;
    const auto status = LiPyrowaveReassemblyPushPacket(
      &state, result.bitstream.view().data() + span.offset, span.size, 1000 + index, 100000);
     EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                 status == LI_PYROWAVE_REASSEMBLY_COMPLETE);
  }
  EXPECT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  std::vector<std::uint8_t> output(source->size());
  std::size_t output_length = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(&state, output.data(), output.size(),
                                          &output_length, nullptr),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(output_length, source->size());
  EXPECT_EQ(output, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, RecoversMissingBlocksAcrossFecGroupsAndKeepsTail) {
  constexpr std::size_t payload_boundary = 1200;
  constexpr std::size_t data_block_count = LI_PYROWAVE_FEC_DATA_PER_GROUP * 2 + 3;
  const auto source_size = payload_boundary * data_block_count - 17;
  auto source = std::make_shared<std::vector<std::uint8_t>>(source_size);
  for (std::size_t index = 0; index < source->size(); ++index) {
    (*source)[index] = static_cast<std::uint8_t>((index * 37 + 11) & 0xff);
  }
  std::shared_ptr<const std::vector<std::uint8_t>> source_view = source;

  pyrowave::encoded_frame_t frame {
    .frame_id = 9,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source_view },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = payload_boundary + LI_PYROWAVE_WIRE_FEC_HEADER_SIZE,
    .rtp_timestamp = 6789,
    .block_aware_fec = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
   ASSERT_EQ(result.packets.size(), data_block_count + 3u + 1u);

  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (std::size_t index = result.packets.size(); index-- > 0;) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto span = result.packets[index];
    const auto *packet = result.bitstream.view().data() + span.offset;
    ASSERT_EQ(LiPyrowaveParsePacket(packet, span.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
    if ((header.flags & LI_PYROWAVE_FLAG_FEC_PARITY) == 0 &&
        (header.blockIndex == 1 || header.blockIndex == 34)) {
      continue;
    }

    const auto status = LiPyrowaveReassemblyPushPacket(
      &state, packet, span.size, 2000 + index, 100000);
    EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                status == LI_PYROWAVE_REASSEMBLY_COMPLETE ||
                status == LI_PYROWAVE_REASSEMBLY_DUPLICATE)
      << "status=" << static_cast<int>(status) << ", packet_index=" << index;
  }

  ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  std::vector<std::uint8_t> output(source_size);
  std::size_t output_length = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(&state, output.data(), output.size(),
                                          &output_length, nullptr),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(output_length, source_size);
  EXPECT_EQ(output, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, DoesNotRecoverTwoMissingBlocksFromOneParityShard) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    std::vector<std::uint8_t>(5000, 0x3c));
  pyrowave::encoded_frame_t frame {
    .frame_id = 10,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1240,
    .rtp_timestamp = 6790,
    .block_aware_fec = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (std::size_t index = 0; index < result.packets.size(); ++index) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto span = result.packets[index];
    const auto *packet = result.bitstream.view().data() + span.offset;
    ASSERT_EQ(LiPyrowaveParsePacket(packet, span.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
    if ((header.flags & LI_PYROWAVE_FLAG_FEC_PARITY) == 0 &&
        (header.blockIndex == 1 || header.blockIndex == 2)) {
      continue;
    }
    LiPyrowaveReassemblyPushPacket(&state, packet, span.size, 3000 + index, 100000);
  }

  EXPECT_FALSE(LiPyrowaveReassemblyIsComplete(&state));
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, AcceptsReorderedPacketsAndRejectsDuplicates) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    std::vector<std::uint8_t>(5000, 0x4d));
  pyrowave::encoded_frame_t frame {
    .frame_id = 11,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1240,
    .rtp_timestamp = 6791,
    .block_aware_fec = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
   ASSERT_EQ(result.packets.size(), 7u);
  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);

   const std::size_t order[] = { 2, 2, 4, 0, 5, 1, 3, 6 };
  for (std::size_t position = 0; position < std::size(order); ++position) {
    const auto span = result.packets[order[position]];
    const auto *packet = result.bitstream.view().data() + span.offset;
    const auto status = LiPyrowaveReassemblyPushPacket(
      &state, packet, span.size, 4000 + position, 100000);
    if (position == 1) {
      EXPECT_EQ(status, LI_PYROWAVE_REASSEMBLY_DUPLICATE);
    }
    else {
      EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                  status == LI_PYROWAVE_REASSEMBLY_COMPLETE ||
                  status == LI_PYROWAVE_REASSEMBLY_DUPLICATE)
        << "status=" << static_cast<int>(status) << ", packet_index=" << order[position];
    }
  }

  EXPECT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, RejectsBoundariesThatCannotCarryWireHeaders) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    std::vector<std::uint8_t>(16, 0x55));
  pyrowave::encoded_frame_t frame {
    .frame_id = 12,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();

  const auto result = packetizer->packetize(frame, {
    .packet_boundary = LI_PYROWAVE_WIRE_FEC_HEADER_SIZE,
    .block_aware_fec = true,
  });
  EXPECT_EQ(result.failure, pyrowave::failure_e::configuration_invalid);
  EXPECT_TRUE(result.packets.empty());
  EXPECT_TRUE(result.bitstream.empty());
}

TEST(PyrowavePacketTest, FailedPacketizationDoesNotExposePayload) {
  pyrowave::packetization_result_t result;
  result.failure = pyrowave::failure_e::packetization_failed;
  result.bitstream.storage = std::make_shared<std::vector<std::uint8_t>>(
    std::initializer_list<std::uint8_t> { 1 });

  EXPECT_FALSE(pyrowave::validate(result));
  result.bitstream.storage.reset();
  EXPECT_TRUE(pyrowave::validate(result));
}

TEST(PyrowaveRuntimeTest, ExposesFormatOnlyOnSupportedServerPlatform) {
  EXPECT_TRUE(pyrowave::is_experimental_video_format(LI_PYROWAVE_VIDEO_FORMAT));
#ifdef _WIN32
  EXPECT_TRUE(pyrowave::is_server_video_format_available(LI_PYROWAVE_VIDEO_FORMAT));
  EXPECT_FALSE(pyrowave::is_server_video_format_available(0u));
  EXPECT_FALSE(pyrowave::is_server_video_format_available(1u));
  EXPECT_FALSE(pyrowave::is_server_video_format_available(2u));
  EXPECT_FALSE(pyrowave::is_server_video_format_available(UINT32_MAX));
#else
  EXPECT_FALSE(pyrowave::is_server_video_format_available(LI_PYROWAVE_VIDEO_FORMAT));
  EXPECT_TRUE(pyrowave::is_server_video_format_available(0u));
  EXPECT_TRUE(pyrowave::is_server_video_format_available(1u));
  EXPECT_TRUE(pyrowave::is_server_video_format_available(2u));
  EXPECT_TRUE(pyrowave::is_server_video_format_available(UINT32_MAX));
#endif
}

TEST(PyrowavePacketTest, RejectsNegativeReassemblyDeadline) {
  pyrowave::reassembly_policy_t policy;
  policy.deadline = std::chrono::milliseconds { -1 };

  EXPECT_FALSE(pyrowave::validate(policy));
}

TEST(PyrowaveGpuInteropTest, RequiresAnImageAndValidDimensions) {
  pyrowave::gpu_frame_t frame;
  frame.width = 1920;
  frame.height = 1080;
  frame.format = pyrowave::image_format_e::nv12;
  EXPECT_FALSE(pyrowave::validate(frame));

  frame.opaque_image = &frame;
  EXPECT_TRUE(pyrowave::validate(frame));
  frame.width = 1919;
  EXPECT_FALSE(pyrowave::validate(frame));
}

TEST(PyrowaveSessionTest, ValidatesLifecycle) {
  pyrowave::session_t session { 7 };
  EXPECT_EQ(session.start(), pyrowave::failure_e::invalid_state);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::failed);

  EXPECT_EQ(session.negotiate(enabled_config(), supported_device(), supported_client()), pyrowave::failure_e::none);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::prepared);
  EXPECT_EQ(session.start(), pyrowave::failure_e::none);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::streaming);

  session.stop();
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::idle);
  session.stop();
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::idle);
}

TEST(PyrowaveSessionTest, FailedSessionCanNegotiateAgain) {
  pyrowave::session_t session { 8 };
  EXPECT_EQ(session.start(), pyrowave::failure_e::invalid_state);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::failed);

  EXPECT_EQ(session.negotiate(enabled_config(), supported_device(), supported_client()), pyrowave::failure_e::none);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::prepared);
}

TEST(PyrowaveEncodeGuardTest, PreservesReturnCodesWithoutDiagnostics) {
  int failures = 0;
  for (int code : { 0, 7, -8 }) {
    EXPECT_EQ(pyrowave::guarded_encode([&] { return code; }, [&](std::exception_ptr) { ++failures; }), code);
  }
  EXPECT_EQ(failures, 0);
}

TEST(PyrowaveEncodeGuardTest, ConvertsBadAllocAndReportsOriginalExceptionOnce) {
  const auto original = std::make_exception_ptr(std::bad_alloc {});
  int failures = 0;
  EXPECT_EQ(pyrowave::guarded_encode([&]() -> int { std::rethrow_exception(original); }, [&](std::exception_ptr error) {
    ++failures;
    EXPECT_EQ(error, original);
    EXPECT_THROW(std::rethrow_exception(error), std::bad_alloc);
  }), -1);
  EXPECT_EQ(failures, 1);
}

TEST(PyrowaveEncodeGuardTest, ConvertsUnknownExceptionAndReportsOriginalOnce) {
  const auto original = std::make_exception_ptr(42);
  int failures = 0;
  EXPECT_EQ(pyrowave::guarded_encode([&]() -> int { std::rethrow_exception(original); }, [&](std::exception_ptr error) {
    ++failures;
    EXPECT_EQ(error, original);
    try {
      std::rethrow_exception(error);
    }
    catch (int value) {
      EXPECT_EQ(value, 42);
    }
  }), -1);
  EXPECT_EQ(failures, 1);
}

TEST(PyrowaveEncodeGuardTest, SwallowsThrowingDiagnostics) {
  int failures = 0;
  EXPECT_EQ(pyrowave::guarded_encode([]() -> int { throw std::bad_alloc {}; }, [&](std::exception_ptr error) {
    ++failures;
    EXPECT_THROW(std::rethrow_exception(error), std::bad_alloc);
    throw 99;
  }), -1);
  EXPECT_EQ(failures, 1);
}

namespace {
  class PyrowaveBoundedWaitTest: public ::testing::Test {
  protected:
    using result = pyrowave::wait_result;
    std::chrono::steady_clock::time_point time {};
    int calls = 0;
    bool stopped = false;
    template<class Poll>
    result run(Poll poll) {
      return pyrowave::bounded_wait([&] {
        ++calls;
        time += std::chrono::milliseconds(1);
        return poll();
      }, [&] { return stopped; }, [&] { return time; }, std::chrono::seconds(1));
    }
  };

  TEST_F(PyrowaveBoundedWaitTest, ImmediateComplete) {
    EXPECT_EQ(run([] { return result::complete; }), result::complete);
    EXPECT_EQ(calls, 1);
  }
  TEST_F(PyrowaveBoundedWaitTest, PendingThenComplete) {
    EXPECT_EQ(run([&] { return calls == 3 ? result::complete : result::pending; }), result::complete);
    EXPECT_EQ(calls, 3);
  }
  TEST_F(PyrowaveBoundedWaitTest, PermanentPendingDeadline) {
    EXPECT_EQ(run([] { return result::pending; }), result::failed);
    EXPECT_EQ(time.time_since_epoch(), std::chrono::seconds(1));
    EXPECT_EQ(calls, 1000);  // No poll after the total pending deadline.
  }
  TEST_F(PyrowaveBoundedWaitTest, ImmediateFailure) {
    EXPECT_EQ(run([] { return result::failed; }), result::failed);
    EXPECT_EQ(calls, 1);
  }
  TEST_F(PyrowaveBoundedWaitTest, StopBeforePoll) {
    stopped = true;
    EXPECT_EQ(run([] { return result::complete; }), result::failed);
    EXPECT_EQ(calls, 0);
  }
  TEST_F(PyrowaveBoundedWaitTest, StopAfterPending) {
    EXPECT_EQ(run([&] { stopped = true; return result::pending; }), result::failed);
    EXPECT_EQ(calls, 1);
  }
  TEST_F(PyrowaveBoundedWaitTest, StopRacesCompletion) {
    EXPECT_EQ(run([&] { stopped = true; return result::complete; }), result::failed);
    EXPECT_EQ(calls, 1);
  }
}  // namespace
