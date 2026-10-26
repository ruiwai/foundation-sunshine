#pragma once

#include <atomic>

namespace pyrowave {
  // Reconfiguration holds admission even if the active session is released.
  class video_admission_t {
  public:
    bool
    try_acquire() noexcept {
      unsigned expected = 0;
      return state_.compare_exchange_strong(expected, active_bit, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool
    release() noexcept {
      return (state_.fetch_and(~active_bit, std::memory_order_acq_rel) & active_bit) != 0;
    }

    bool
    active() const noexcept {
      return (state_.load(std::memory_order_acquire) & active_bit) != 0;
    }

    bool
    try_begin_reconfiguration() noexcept {
      unsigned expected = active_bit;
      return state_.compare_exchange_strong(expected, active_bit | reconfiguring_bit, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void
    end_reconfiguration() noexcept {
      state_.fetch_and(~reconfiguring_bit, std::memory_order_release);
    }

  private:
    static constexpr unsigned active_bit = 1;
    static constexpr unsigned reconfiguring_bit = 2;
    std::atomic<unsigned> state_ {0};
  };
}  // namespace pyrowave
