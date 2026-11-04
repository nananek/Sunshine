/**
 * @file src/kmd.h
 * @brief Deliver Moonlight input to a remote KVM gateway instead of the local HID stack.
 */
#pragma once

#include <cstdint>

namespace kmd {
  /**
   * @brief True when SUNSHINE_KMD_SOCKET points at a serial-kmd instance.
   */
  bool enabled();

  /**
   * @brief Press or release a key.
   *
   * @param vk Windows virtual-key code as delivered by the client.
   * @param release True for a key release.
   * @param flags Client modifier flags (side information).
   * @param extended True when the client marked the key as extended.
   */
  void key(std::uint16_t vk, bool release, std::uint8_t flags, bool extended);

  /**
   * @brief Type literal text (used by clients that send unicode input).
   */
  void text(const char *utf8, int size);

  /**
   * @brief Move the pointer by a delta.
   */
  void move_mouse(int delta_x, int delta_y);

  /**
   * @brief Move the pointer to a normalized position.
   */
  void abs_mouse(float x, float y);

  /**
   * @brief Press or release a mouse button.
   */
  void button(int button, bool release);

  /**
   * @brief Scroll the wheel.
   *
   * @param high_res_distance Signed distance in high resolution wheel units.
   */
  void scroll(int high_res_distance);

  /**
   * @brief Release everything this session holds.
   */
  void reset();
}
