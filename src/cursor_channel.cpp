/**
 * @file src/cursor_channel.cpp
 * @brief See cursor_channel.h.
 */
#include "cursor_channel.h"

#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cursor_channel {
  namespace {
    std::mutex state_mutex;
    std::unordered_set<std::uint32_t> enabled_sessions;
    std::unordered_map<std::uint32_t, std::uint32_t> session_owners;
    struct producer_t {
      bool available = false;
      snapshot_t latest;
    };
    std::unordered_map<std::uint32_t, producer_t> producers;
    thread_local std::uint32_t publisher_owner = 0;
    // A 64-bit revision cannot practically wrap within a process lifetime.
    std::uint64_t next_revision = 1;

    std::uint32_t
    owner_for(std::uint32_t session_id) {
      auto it = session_owners.find(session_id);
      return it == session_owners.end() ? 0 : it->second;
    }

    bool
    owner_enabled(std::uint32_t owner) {
      for (auto session_id : enabled_sessions) {
        if (owner_for(session_id) == owner) return true;
      }
      return false;
    }

    void
    set_enabled(std::uint32_t session_id, bool enabled) {
      const auto owner = owner_for(session_id);
      const bool was_enabled = owner_enabled(owner);
      if (enabled) enabled_sessions.insert(session_id);
      else enabled_sessions.erase(session_id);
      if ((!was_enabled && enabled) || !owner_enabled(owner)) {
        auto it = producers.find(owner);
        if (it != producers.end()) it->second.latest = {};
      }
    }
  }  // namespace

  void
  register_private_session(std::uint32_t session_id) {
    if (!session_id) throw std::invalid_argument("Private cursor session ID must be nonzero");
    std::lock_guard lock(state_mutex);
    if (!session_owners.emplace(session_id, session_id).second) {
      throw std::logic_error("Private cursor session already registered");
    }
  }

  producer_scope::producer_scope(std::uint32_t session_id):
      owner_(session_id), previous_owner_(publisher_owner) {
    std::lock_guard lock(state_mutex);
    if (!owner_ || owner_for(session_id) != owner_) {
      throw std::logic_error("Private cursor session is not registered");
    }
    if (!producers.try_emplace(owner_).second) {
      throw std::logic_error("Private cursor producer already active");
    }
    publisher_owner = owner_;
  }

  producer_scope::~producer_scope() {
    std::lock_guard lock(state_mutex);
    producers.erase(owner_);
    // Retain the binding until control removes the session: never fall back
    // to a live legacy producer while this private session is tearing down.
    publisher_owner = previous_owner_;
  }

  void
  set_session_enabled(std::uint32_t session_id, bool enabled) {
    std::lock_guard lock(state_mutex);
    set_enabled(session_id, enabled);
  }

  void
  remove_session(std::uint32_t session_id) {
    std::lock_guard lock(state_mutex);
    set_enabled(session_id, false);
    session_owners.erase(session_id);
  }

  bool
  local_mode_active() {
    std::lock_guard lock(state_mutex);
    return !enabled_sessions.empty();
  }

  bool
  producer_local_mode_active() {
    std::lock_guard lock(state_mutex);
    return owner_enabled(publisher_owner);
  }

  void
  set_producer_available(bool available) {
    std::lock_guard lock(state_mutex);
    producers[publisher_owner].available = available;
  }

  bool
  producer_available(std::uint32_t session_id) {
    std::lock_guard lock(state_mutex);
    auto it = producers.find(owner_for(session_id));
    return it != producers.end() && it->second.available;
  }

  void
  publish_visibility(bool visible, std::uint32_t shape_id) {
    std::lock_guard lock(state_mutex);
    if (!owner_enabled(publisher_owner)) {
      return;
    }
    auto &latest = producers[publisher_owner].latest;
    if (latest.valid && latest.visible == visible && latest.shape_id == shape_id) {
      return;
    }

    if (latest.shape_id != shape_id) {
      latest.has_shape = false;
      latest.width = 0;
      latest.height = 0;
      latest.hotspot_x = 0;
      latest.hotspot_y = 0;
      latest.bgra.clear();
    }
    latest.valid = true;
    latest.visible = visible;
    latest.shape_id = shape_id;
    latest.revision = next_revision++;
  }

  void
  publish_shape(bool visible,
                std::uint32_t shape_id,
                std::uint16_t width,
                std::uint16_t height,
                std::int16_t hotspot_x,
                std::int16_t hotspot_y,
                std::vector<std::uint8_t> bgra) {
    std::lock_guard lock(state_mutex);
    if (!owner_enabled(publisher_owner)) {
      return;
    }
    auto &latest = producers[publisher_owner].latest;

    latest.valid = true;
    latest.visible = visible;
    latest.has_shape = true;
    latest.shape_id = shape_id;
    latest.width = width;
    latest.height = height;
    latest.hotspot_x = hotspot_x;
    latest.hotspot_y = hotspot_y;
    latest.bgra = std::move(bgra);
    latest.revision = next_revision++;
  }

  bool
  copy_latest(std::uint64_t known_revision, snapshot_t &out, std::uint32_t session_id) {
    std::lock_guard lock(state_mutex);
    auto it = producers.find(owner_for(session_id));
    if (it == producers.end()) return false;
    const auto &latest = it->second.latest;
    if (!latest.valid || latest.revision == known_revision) {
      return false;
    }
    out = latest;
    return true;
  }
}  // namespace cursor_channel
