#pragma once

#include <chrono>

namespace pyrowave {
  enum class wait_result { pending, complete, failed };

  // Poll must itself be bounded: production native waits block for 1 ms, not spin.
  // Driver calls are not cancellable; this only bounds host pending-wait polling.
  template<class Poll, class Stop, class Now>
  wait_result
  bounded_wait(Poll poll, Stop &&stop, Now now, std::chrono::steady_clock::duration timeout) {
    const auto deadline = now() + timeout;
    for (;;) {
      if (stop()) return wait_result::failed;
      const auto result = poll();
      if (result != wait_result::pending) return stop() ? wait_result::failed : result;
      if (now() >= deadline) return wait_result::failed;
    }
  }
}  // namespace pyrowave
