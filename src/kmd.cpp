/**
 * @file src/kmd.cpp
 * @brief Deliver Moonlight input to serial-kmd instead of the local HID stack.
 *
 * A host without its own display cannot press keys on the machine being
 * streamed: the pointer and keyboard must reach the target through the KVM
 * gateway that owns it. That gateway answers one request at a time over a
 * 115200 baud link, so a round trip costs several milliseconds — a report per
 * input sample would spend the whole link on pointer movement and fall behind.
 * Requests are queued for a writer thread, and pointer movement is accumulated
 * and emitted at a fixed rate instead.
 */
#include "src/kmd.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/logging.h"

namespace kmd {
  namespace {
    /**
     * @brief A queued request that must be delivered as-is (keys, text, wheel).
     */
    struct item_t {
      std::string line;
    };

    constexpr auto RECEIVE_TIMEOUT = 200;  ///< Milliseconds to wait for an ack.
    constexpr std::size_t MAX_QUEUE = 64;  ///< Requests allowed to wait.
    constexpr auto KEEPALIVE = std::chrono::seconds(1);  ///< Held-state refresh.

    // The link carries roughly 11.5 bytes per millisecond and each pointer
    // report is a request plus an ack, so the pointer is capped instead of
    // tracking the client one to one.
    constexpr auto DEFAULT_POINTER_INTERVAL = std::chrono::milliseconds(12);

    std::mutex state_mutex;  ///< Guards everything below.

    // Keys currently down, by protocol name. The gateway keyboard report is a
    // snapshot, so every change is sent as the complete set.
    std::set<std::string> held_keys;

    // The gateway takes one button name, not a mask. Track the most recent
    // press and send that while it is held.
    std::string held_button;

    // Accumulated pointer movement since the last report.
    int pending_dx {0};
    int pending_dy {0};
    float pending_x {0.5f};
    float pending_y {0.5f};
    bool pointer_absolute {false};
    bool pointer_dirty {false};
    std::chrono::steady_clock::time_point last_pointer {};
    std::chrono::steady_clock::time_point last_request {};
    std::chrono::milliseconds pointer_interval {DEFAULT_POINTER_INTERVAL};

    std::deque<item_t> queue;
    std::condition_variable queue_cv;
    bool worker_started {false};
    bool stopping {false};
    std::thread worker;

    int socket_fd {-1};

    /**
     * @brief Map a Windows virtual-key code to a gateway key name.
     */
    const char *key_name(std::uint16_t vk) {
      if (vk >= 0x41 && vk <= 0x5A) {
        static const char *letters[] = {
          "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
          "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
        };
        return letters[vk - 0x41];
      }
      if (vk >= 0x30 && vk <= 0x39) {
        static const char *digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        return digits[vk - 0x30];
      }
      if (vk >= 0x70 && vk <= 0x7B) {
        static const char *function_keys[] = {
          "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
        };
        return function_keys[vk - 0x70];
      }
      if (vk >= 0x60 && vk <= 0x69) {
        static const char *keypad[] = {
          "kp0", "kp1", "kp2", "kp3", "kp4", "kp5", "kp6", "kp7", "kp8", "kp9",
        };
        return keypad[vk - 0x60];
      }

      switch (vk) {
        case 0x08: return "backspace";
        case 0x09: return "tab";
        case 0x0D: return "enter";
        case 0x1B: return "esc";
        case 0x20: return "space";
        case 0x10: return "shift";
        case 0x11: return "ctrl";
        case 0x12: return "alt";
        case 0x5B: return "leftgui";
        case 0x5C: return "rightgui";
        case 0x5D: return "application";
        case 0x13: return "pause";
        case 0x14: return "capslock";
        case 0x21: return "pageup";
        case 0x22: return "pagedown";
        case 0x23: return "end";
        case 0x24: return "home";
        case 0x25: return "left";
        case 0x26: return "up";
        case 0x27: return "right";
        case 0x28: return "down";
        case 0x2C: return "printscreen";
        case 0x2D: return "insert";
        case 0x2E: return "delete";
        case 0x6A: return "kpasterisk";
        case 0x6B: return "kpplus";
        case 0x6D: return "kpminus";
        case 0x6E: return "kpdot";
        case 0x6F: return "kpslash";
        case 0x90: return "numlock";
        case 0x91: return "scrolllock";
        case 0xA0: return "leftshift";
        case 0xA1: return "rightshift";
        case 0xA2: return "leftctrl";
        case 0xA3: return "rightctrl";
        case 0xA4: return "leftalt";
        case 0xA5: return "rightalt";
        case 0xBA: return "semicolon";
        case 0xBB: return "equal";
        case 0xBC: return "comma";
        case 0xBD: return "minus";
        case 0xBE: return "period";
        case 0xBF: return "slash";
        case 0xC0: return "grave";
        case 0xDB: return "lbracket";
        case 0xDC: return "backslash";
        case 0xDD: return "rbracket";
        case 0xDE: return "apostrophe";
        case 0xE2: return "nonusbackslash";
        // Japanese layout
        case 0x15: return "kana";
        case 0x19: return "zenkaku";
        case 0x1C: return "henkan";
        case 0x1D: return "muhenkan";
        default: return nullptr;
      }
    }

    void close_socket() {
      if (socket_fd >= 0) {
        ::close(socket_fd);
        socket_fd = -1;
      }
    }

    bool open_socket() {
      const char *env = std::getenv("SUNSHINE_KMD_SOCKET");
      if (!env || !*env) {
        return false;
      }

      socket_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (socket_fd < 0) {
        return false;
      }

      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      const auto path_len = std::strlen(env);
      if (path_len >= sizeof(addr.sun_path)) {
        close_socket();
        return false;
      }
      std::memcpy(addr.sun_path, env, path_len);

      if (::connect(socket_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close_socket();
        return false;
      }

      timeval timeout {};
      timeout.tv_sec = 0;
      timeout.tv_usec = RECEIVE_TIMEOUT * 1000;
      ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      return true;
    }

    /**
     * @brief Send one request. Writer thread only.
     *
     * Pointer reports skip the wait: at 115200 baud the round trip costs more
     * than the report, and a stale ack is harmless. Acks that accumulate are
     * drained on the next send so the daemon never blocks on a full socket.
     */
    bool send_request(const std::string &line, bool wait_ack = true) {
      if (socket_fd < 0 && !open_socket()) {
        return false;
      }

      std::string payload = line;
      payload += '\n';
      if (::send(socket_fd, payload.data(), payload.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(payload.size())) {
        close_socket();
        return false;
      }

      char response[1024];

      if (!wait_ack) {
        for (;;) {
          const auto ready = ::recv(socket_fd, response, sizeof(response), MSG_DONTWAIT);
          if (ready > 0) {
            continue;
          }
          if (ready == 0) {
            close_socket();
            return false;
          }
          return errno == EAGAIN || errno == EWOULDBLOCK;
        }
      }

      const auto len = ::recv(socket_fd, response, sizeof(response) - 1, 0);
      if (len <= 0) {
        close_socket();
        return false;
      }
      return true;
    }

    std::string escape(const char *text, int size) {
      std::string out;
      out.reserve(size + 8);
      for (int i = 0; i < size; ++i) {
        const char c = text[i];
        switch (c) {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\n': out += "\\n"; break;
          case '\t': out += "\\t"; break;
          default:
            if (static_cast<unsigned char>(c) < 0x20) {
              char buf[8];
              std::snprintf(buf, sizeof(buf), "\\u%04x", c);
              out += buf;
            }
            else {
              out += c;
            }
        }
      }
      return out;
    }

    /**
     * @brief Build the report for the accumulated pointer state.
     */
    std::string render_pointer_locked() {
      char buf[192];

      if (pointer_absolute) {
        if (held_button.empty()) {
          std::snprintf(buf, sizeof(buf), "{\"op\":\"mouse_abs\",\"x\":%.4f,\"y\":%.4f}", pending_x, pending_y);
        }
        else {
          std::snprintf(buf, sizeof(buf), "{\"op\":\"mouse_abs\",\"x\":%.4f,\"y\":%.4f,\"btn\":\"%s\"}", pending_x, pending_y, held_button.c_str());
        }
      }
      else if (held_button.empty()) {
        std::snprintf(buf, sizeof(buf), "{\"op\":\"mouse_rel\",\"dx\":%d,\"dy\":%d}", pending_dx, pending_dy);
      }
      else {
        std::snprintf(buf, sizeof(buf), "{\"op\":\"mouse_rel\",\"dx\":%d,\"dy\":%d,\"btn\":\"%s\"}", pending_dx, pending_dy, held_button.c_str());
      }
      return buf;
    }

    void writer_loop() {
      std::unique_lock lk {state_mutex};
      last_request = std::chrono::steady_clock::now();
      last_pointer = last_request;

      for (;;) {
        if (stopping && queue.empty() && !pointer_dirty) {
          return;
        }

        if (!queue.empty()) {
          item_t item = std::move(queue.front());
          queue.pop_front();
          lk.unlock();
          send_request(item.line, true);
          lk.lock();
          last_request = std::chrono::steady_clock::now();
          continue;
        }

        if (pointer_dirty) {
          const auto now = std::chrono::steady_clock::now();
          const auto due = last_pointer + pointer_interval;
          if (now < due) {
            queue_cv.wait_until(lk, due);
            continue;
          }

          std::string line = render_pointer_locked();
          pointer_dirty = false;
          pending_dx = 0;
          pending_dy = 0;
          last_pointer = now;
          last_request = now;

          lk.unlock();
          send_request(line, false);
          lk.lock();
          continue;
        }

        if (stopping) {
          return;
        }

        // Idle. A held key or button is only maintained by traffic: the gateway
        // releases everything when it stops hearing from us.
        const auto holding = !held_keys.empty() || !held_button.empty();
        if (!holding) {
          queue_cv.wait(lk, [] { return stopping || !queue.empty() || pointer_dirty; });
          continue;
        }

        const auto next = last_request + KEEPALIVE;
        if (std::chrono::steady_clock::now() >= next) {
          lk.unlock();
          send_request("{\"op\":\"ping\"}");
          lk.lock();
          last_request = std::chrono::steady_clock::now();
          continue;
        }
        queue_cv.wait_until(lk, next);
      }
    }

    void start_worker_locked() {
      if (!worker_started) {
        worker_started = true;
        worker = std::thread {writer_loop};
      }
    }

    void touch_pointer_locked();

    void enqueue_locked(std::string line) {
      start_worker_locked();

      while (queue.size() >= MAX_QUEUE) {
        queue.pop_front();
      }
      queue.push_back(item_t {std::move(line)});
      queue_cv.notify_one();
    }

    /**
     * @brief Queue the current key set as one snapshot report.
     */
    void sync_keys_locked() {
      // hold_ms 0 はゲートウェイが bad_value で拒否する。1 が最小。
      std::string line = "{\"op\":\"key\",\"hold\":true,\"hold_ms\":1,\"chord\":[";
      bool first = true;
      for (const auto &name : held_keys) {
        if (!first) {
          line += ',';
        }
        line += '"';
        line += name;
        line += '"';
        first = false;
      }
      if (held_keys.empty()) {
        // 空の chord も受け付けないので、最後のキーは全解放で離す。
        enqueue_locked("{\"op\":\"release_all\"}");
        if (!held_button.empty()) {
          // 全解放で落ちたボタンを押し直す。
          touch_pointer_locked();
        }
        return;
      }

      line += "]}";
      enqueue_locked(std::move(line));
    }

    /**
     * @brief Mark the pointer dirty so the next slot reports the new state.
     */
    void touch_pointer_locked() {
      start_worker_locked();
      pointer_dirty = true;
      queue_cv.notify_one();
    }

    struct shutdown_t {
      ~shutdown_t() {
        {
          std::lock_guard lg {state_mutex};
          stopping = true;
        }
        queue_cv.notify_all();
        if (worker.joinable()) {
          worker.join();
        }
        close_socket();
      }
    } shutdown;
  }  // namespace

  bool enabled() {
    const char *env = std::getenv("SUNSHINE_KMD_SOCKET");
    return env && *env;
  }

  void key(std::uint16_t vk, bool release, std::uint8_t flags, bool extended) {
    (void) flags;
    (void) extended;

    const char *name = key_name(vk);
    if (!name) {
      BOOST_LOG(debug) << "kmd: unmapped key code " << vk;
      return;
    }

    std::lock_guard lg {state_mutex};
    if (release) {
      held_keys.erase(name);
    }
    else {
      held_keys.insert(name);
    }
    sync_keys_locked();
  }

  void text(const char *utf8, int size) {
    if (!utf8 || size <= 0) {
      return;
    }

    std::lock_guard lg {state_mutex};
    enqueue_locked("{\"op\":\"type\",\"text\":\"" + escape(utf8, size) + "\"}");
  }

  void move_mouse(int delta_x, int delta_y) {
    if (!delta_x && !delta_y) {
      return;
    }

    std::lock_guard lg {state_mutex};
    if (pointer_absolute) {
      // The client switched to relative reporting; drop the stale position.
      pointer_absolute = false;
    }
    pending_dx += delta_x;
    pending_dy += delta_y;
    touch_pointer_locked();
  }

  void abs_mouse(float x, float y) {
    std::lock_guard lg {state_mutex};
    pointer_absolute = true;
    pending_x = std::clamp(x, 0.0f, 1.0f);
    pending_y = std::clamp(y, 0.0f, 1.0f);
    touch_pointer_locked();
  }

  void button(int button_code, bool release) {
    std::lock_guard lg {state_mutex};

    const char *name = nullptr;
    switch (button_code) {
      case 1: name = "left"; break;
      case 2: name = "middle"; break;
      case 3: name = "right"; break;
      default: return;
    }

    if (release) {
      if (held_button != name) {
        // A different button is the one this gateway can express; keep it down.
        return;
      }
      held_button.clear();
    }
    else {
      held_button = name;
    }

    touch_pointer_locked();
  }

  void scroll(int high_res_distance) {
    if (!high_res_distance) {
      return;
    }

    // One notch is 120 units in the client's high resolution reporting.
    int amount = high_res_distance / 120;
    if (!amount) {
      amount = high_res_distance > 0 ? 1 : -1;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"op\":\"scroll\",\"amount\":%d}", amount);

    std::lock_guard lg {state_mutex};
    enqueue_locked(buf);
  }

  void reset() {
    std::lock_guard lg {state_mutex};
    held_keys.clear();
    held_button.clear();
    pointer_dirty = false;
    pending_dx = 0;
    pending_dy = 0;
    enqueue_locked("{\"op\":\"release_all\"}");
  }
}  // namespace kmd
