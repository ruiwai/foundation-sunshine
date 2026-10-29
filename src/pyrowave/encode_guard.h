/**
 * @file src/pyrowave/encode_guard.h
 * @brief Exception boundary for frame encoding and its failure diagnostics.
 */
#pragma once

#include <exception>

namespace pyrowave {
  template<class Callback, class FailureCallback>
  int
  guarded_encode(Callback &&callback, FailureCallback &&failure_callback) noexcept {
    try {
      return callback();
    }
    catch (...) {
      try {
        failure_callback(std::current_exception());
      }
      catch (...) {}
      return -1;
    }
  }
}  // namespace pyrowave
