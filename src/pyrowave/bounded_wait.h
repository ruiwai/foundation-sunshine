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

  // Signals only wake the host; poll remains the completion proof.
  template<class Poll, class Arm, class Wait, class Stop, class Now>
  wait_result
  bounded_event_wait(Poll poll, Arm arm, Wait wait, Stop &&stop, Now now, std::chrono::steady_clock::duration timeout) {
    const auto deadline = now() + timeout;
    bool armed = false;
    for (;;) {
      if (stop()) return wait_result::failed;
      const auto result = poll();
      if (result != wait_result::pending) return stop() ? wait_result::failed : result;
      if (now() >= deadline) return wait_result::failed;
      if (!armed) {
        if (!arm()) return wait_result::failed;
        armed = true;
      }
      if (stop()) return wait_result::failed;
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now());
      if (remaining <= std::chrono::milliseconds::zero()) return wait_result::failed;
      const auto slice = remaining < std::chrono::milliseconds(10) ? remaining : std::chrono::milliseconds(10);
      if (!wait(slice)) return wait_result::failed;
    }
  }
}  // namespace pyrowave
