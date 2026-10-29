/** Latest display request, independently observed by each capture owner. */
#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace capture_control {
  class display_switch_t {
  public:
    void request(int index) {
      std::lock_guard lock(mutex_);
      if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Display switch generation exhausted");
      }
      index_ = index;
      ++generation_;
    }

    std::uint64_t generation() const {
      std::lock_guard lock(mutex_);
      return generation_;
    }

    bool pending(std::uint64_t seen) const {
      std::lock_guard lock(mutex_);
      return seen != generation_;
    }

    std::optional<int> poll(std::uint64_t &seen) const {
      std::lock_guard lock(mutex_);
      if (seen == generation_) {
        return std::nullopt;
      }
      seen = generation_;
      return index_;
    }

  private:
    mutable std::mutex mutex_;
    std::uint64_t generation_ = 0;
    int index_ = 0;
  };

  inline display_switch_t display_switch;
}  // namespace capture_control
