#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace pyrowave {
  // Single-thread owner: no concurrent callers. Cache planes, never bitstreams.
  // finish_encode requires completion of the external release fence first.
  class plane_ring_t {
  public:
    static constexpr std::size_t size = 2;
    enum class state { free, converting, ready, encoding };

    std::optional<std::size_t> begin_conversion() noexcept {
      if (!valid() || busy()) return std::nullopt;
      for (auto &value : states_) {
        if (value == state::ready) value = state::free;
      }
      const auto slot = next_;
      if (states_[slot] != state::free) return std::nullopt;
      if (cached_ == slot) cached_.reset();
      states_[slot] = state::converting;
      next_ = (slot + 1) % size;
      return slot;
    }

    bool finish_conversion(std::size_t slot) noexcept {
      if (!valid() || slot >= size || states_[slot] != state::converting) return false;
      states_[slot] = state::ready;
      return true;
    }

    std::optional<std::size_t> begin_encode() noexcept {
      if (!valid() || busy()) return std::nullopt;
      auto slot = cached_;
      for (std::size_t i = 0; i < size; ++i) {
        if (states_[i] == state::ready) slot = i;
      }
      if (!slot) return std::nullopt;
      states_[*slot] = state::encoding;
      return slot;
    }

    bool finish_encode(std::size_t slot) noexcept {
      if (!valid() || slot >= size || states_[slot] != state::encoding) return false;
      states_[slot] = state::free;
      cached_ = slot;
      return true;
    }

    void invalidate() noexcept { failed_ = true; }
    bool valid() const noexcept { return !failed_; }

  private:
    bool busy() const noexcept {
      for (const auto value : states_) {
        if (value == state::converting || value == state::encoding) return true;
      }
      return false;
    }
    std::array<state, size> states_ {};
    std::optional<std::size_t> cached_;
    std::size_t next_ = 0;
    bool failed_ = false;
  };
}  // namespace pyrowave
