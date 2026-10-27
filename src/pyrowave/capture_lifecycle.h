#pragma once

#include <chrono>
#include <thread>
#include <utility>

namespace pyrowave {
  enum class capture_wait_result { ready, stopped, timed_out };

  // Callbacks must be nonthrowing; referenced context must outlive this owner.
  // Stop is idempotent. Driver calls may block indefinitely: cleanup is always
  // synchronous, never detached or abandoned at a polling deadline.
  template<class Stop>
  class capture_worker {
  public:
    template<class Running, class Work>
    capture_worker(Stop stop, Running running, Work work):
        stop_(std::move(stop)),
        worker_([this, running = std::move(running), work = std::move(work)]() mutable {
          try {
            struct exit_guard {
              Stop &stop;
              ~exit_guard() { stop(); }
            } guard { stop_ };
            if (running()) work();
          } catch (...) {
            // Unwind the exit guard even if a callback violates its contract.
            throw;
          }
        }) {}

    ~capture_worker() {
      stop_();
      if (worker_.joinable()) worker_.join();
    }

    capture_worker(const capture_worker &) = delete;
    capture_worker &operator=(const capture_worker &) = delete;
    capture_worker(capture_worker &&) = delete;
    capture_worker &operator=(capture_worker &&) = delete;

  private:
    Stop stop_;
    std::thread worker_;
  };

  // Drain releases only queued references, never ownership held by another user.
  template<class Ready, class Running, class Drain, class Now, class Pause>
  capture_wait_result
  wait_capture_references(Ready ready, Running running, Drain drain, Now now, Pause pause,
                          std::chrono::steady_clock::duration timeout, bool bounded) {
    const auto deadline = now() + timeout;
    for (;;) {
      if (!running()) return capture_wait_result::stopped;
      if (ready()) return capture_wait_result::ready;
      if (bounded && now() >= deadline) return capture_wait_result::timed_out;
      drain();
      pause();
    }
  }
}  // namespace pyrowave
