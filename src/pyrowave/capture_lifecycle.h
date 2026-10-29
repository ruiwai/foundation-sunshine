#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <thread>
#include <utility>

namespace pyrowave {
  // Only bounds pending host polls, not driver calls or synchronous teardown.
  class pending_deadline {
  public:
    using clock = std::chrono::steady_clock;
    pending_deadline(clock::time_point start, clock::duration timeout):
        timeout_(timeout), deadline_(start + timeout) {}
    void reset(clock::time_point now) { deadline_ = now + timeout_; }
    bool expired(clock::time_point now) const { return now >= deadline_; }

  private:
    clock::duration timeout_;
    clock::time_point deadline_;
  };

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
}  // namespace pyrowave
