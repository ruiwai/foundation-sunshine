/**
 * @file src/cursor_channel.h
 * @brief Latest-value bridge between display capture and the control stream.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace cursor_channel {
  // Admit a nonzero launch ID before exposing control. The private binding
  // outlives producer scopes; only remove_session releases it.
  void
  register_private_session(std::uint32_t session_id);

  class producer_scope {
  public:
    explicit producer_scope(std::uint32_t session_id);
    ~producer_scope();
    producer_scope(const producer_scope &) = delete;
    producer_scope &operator=(const producer_scope &) = delete;
    producer_scope(producer_scope &&) = delete;
    producer_scope &operator=(producer_scope &&) = delete;

  private:
    std::uint32_t owner_;
    std::uint32_t previous_owner_;
  };

  struct snapshot_t {
    std::uint64_t revision = 0;
    bool valid = false;
    bool visible = false;
    bool has_shape = false;
    std::uint32_t shape_id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t hotspot_x = 0;
    std::int16_t hotspot_y = 0;
    std::vector<std::uint8_t> bgra;
  };

  void
  set_session_enabled(std::uint32_t session_id, bool enabled);

  void
  remove_session(std::uint32_t session_id);

  bool
  local_mode_active();

  bool
  producer_local_mode_active();

  void
  set_producer_available(bool available);

  bool
  producer_available(std::uint32_t session_id = 0);

  void
  publish_visibility(bool visible, std::uint32_t shape_id);

  void
  publish_shape(bool visible,
                std::uint32_t shape_id,
                std::uint16_t width,
                std::uint16_t height,
                std::int16_t hotspot_x,
                std::int16_t hotspot_y,
                std::vector<std::uint8_t> bgra);

  bool
  copy_latest(std::uint64_t known_revision, snapshot_t &out, std::uint32_t session_id = 0);
}  // namespace cursor_channel
