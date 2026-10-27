#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <thread>
#include <utility>

namespace pyrowave {
  enum class capture_wait_result { ready, stopped, timed_out };

  // Stop must be nonthrowing and idempotent; referenced context must outlive this owner.
  // Running/work failures are contained. Driver calls may block indefinitely:
  // cleanup is synchronous, never detached or abandoned at a polling deadline.
  template<class Stop>
  class capture_worker {
  public:
    template<class Running, class Work>
    capture_worker(Stop stop, Running running, Work work,
                   std::function<void(std::exception_ptr)> on_error = {}):
        stop_(std::move(stop)),
        worker_([this, running = std::move(running), work = std::move(work),
                 on_error = std::move(on_error)]() mutable {
          try {
            struct exit_guard {
              Stop &stop;
              ~exit_guard() { stop(); }
            } guard { stop_ };
            if (running()) work();
          } catch (...) {
            try {
              if (on_error) on_error(std::current_exception());
            } catch (...) {
              // Reporting must not let a worker failure escape the thread.
            }
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
