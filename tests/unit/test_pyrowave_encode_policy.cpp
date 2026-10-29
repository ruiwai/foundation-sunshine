/** @file test_pyrowave_encode_policy.cpp
 * Strict native FP16 eligibility and per-frame exception isolation.
 */
#include "src/pyrowave/encode_guard.h"
#include "src/pyrowave/native_fp16.h"

#include <gtest/gtest.h>

#include <exception>
#include <stdexcept>

TEST(PyrowaveEncodeGuard, NormalResultsPassThroughWithoutDiagnostics) {
  bool diagnosed = false;
  const auto diagnostic = [&](std::exception_ptr) { diagnosed = true; };
  static_assert(noexcept(pyrowave::guarded_encode([] { return 0; }, diagnostic)));
  for (int result : { 0, 42, -1 }) {
    EXPECT_EQ(pyrowave::guarded_encode([&] { return result; }, diagnostic), result);
  }
  EXPECT_FALSE(diagnosed);
}

TEST(PyrowaveEncodeGuard, StandardExceptionIdentityAndPayloadArePreserved) {
  const auto original = std::make_exception_ptr(std::runtime_error("encode failed"));
  std::exception_ptr observed;
  const auto result = pyrowave::guarded_encode([&]() -> int {
    std::rethrow_exception(original);
  }, [&](std::exception_ptr exception) { observed = exception; });
  EXPECT_EQ(result, -1);
  ASSERT_EQ(observed, original);
  try {
    std::rethrow_exception(observed);
  }
  catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "encode failed");
  }
  catch (...) {
    FAIL() << "exception type changed";
  }
}

TEST(PyrowaveEncodeGuard, UnknownExceptionIdentityAndPayloadArePreserved) {
  const auto original = std::make_exception_ptr(17);
  std::exception_ptr observed;
  EXPECT_EQ(pyrowave::guarded_encode([&]() -> int {
    std::rethrow_exception(original);
  }, [&](std::exception_ptr exception) { observed = exception; }), -1);
  ASSERT_EQ(observed, original);
  try {
    std::rethrow_exception(observed);
  }
  catch (int value) {
    EXPECT_EQ(value, 17);
  }
  catch (...) {
    FAIL() << "exception type changed";
  }
}

TEST(PyrowaveEncodeGuard, DiagnosticExceptionsAreAlsoContained) {
  for (bool standard_encode : { false, true }) {
    for (bool standard_diagnostic : { false, true }) {
      int diagnostic_calls = 0;
      EXPECT_EQ(pyrowave::guarded_encode([&]() -> int {
        if (standard_encode) throw std::runtime_error("encode");
        throw 17;
      }, [&](std::exception_ptr exception) {
        ++diagnostic_calls;
        EXPECT_NE(exception, nullptr);
        if (standard_diagnostic) throw std::runtime_error("diagnostic");
        throw 23;
      }), -1);
      EXPECT_EQ(diagnostic_calls, 1);
    }
  }
}

TEST(PyrowaveNativeFp16Policy, OnlySupportedEightBitWithoutHdrOrOverrideIsEligible) {
  static_assert(pyrowave::use_native_fp16(8, 0, true, false));
  EXPECT_TRUE(pyrowave::use_native_fp16(8, 0, true, false));
  // HDR10/PQ and HLG use 10-bit planes; dynamic metadata also excludes 8-bit.
  for (int depth : { -1, 0, 8, 10, 12, 16 }) {
    for (int hdr : { -1, 0, 1, 2, 3, 4 }) {
      for (bool supported : { false, true }) {
        for (bool overridden : { false, true }) {
          if (depth == 8 && hdr == 0 && supported && !overridden) continue;
          EXPECT_FALSE(pyrowave::use_native_fp16(depth, hdr, supported, overridden))
            << "depth=" << depth << ", hdr=" << hdr << ", supported=" << supported
            << ", overridden=" << overridden;
        }
      }
    }
  }
}
