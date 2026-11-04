/**
 * @file src/proxy.cpp
 * @brief Streams H.264 access units produced elsewhere into the session.
 *
 * A capture device that already encodes H.264 does not need Sunshine's capture
 * and encode stages. This source reads an Annex B stream over a unix socket and
 * publishes access units to the same packet queue the encoders use, so
 * packetization, FEC and the control channel are untouched.
 */

// standard includes
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// lib includes
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// local includes
#include "src/globals.h"
#include "src/logging.h"
#include "src/proxy.h"
#include "src/video.h"

namespace proxy {
  namespace {

    constexpr std::size_t kReadBuffer = 64 * 1024;
    constexpr std::size_t kMaxAccessUnit = 4 * 1024 * 1024;
    constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

    constexpr std::uint8_t kNalAud = 9;
    constexpr std::uint8_t kNalIdr = 5;

    /**
     * @brief Return true when a NAL unit of the given type starts at i.
     */
    bool nal_at(const std::vector<std::uint8_t> &buf, std::size_t i, std::uint8_t type) {
      return i + 3 < buf.size() && buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x01 && (buf[i + 3] & 0x1f) == type;
    }

    /**
     * @brief Find the next start code introducing a NAL unit of the given type.
     *
     * A leading zero of a four byte start code is included so that consecutive
     * access units do not overlap.
     */
    std::size_t find_nal(const std::vector<std::uint8_t> &buf, std::size_t from, std::uint8_t type) {
      for (std::size_t i = from; i + 3 < buf.size(); ++i) {
        if (nal_at(buf, i, type)) {
          if (i > 0 && buf[i - 1] == 0x00) {
            return i - 1;
          }
          return i;
        }
      }
      return kNpos;
    }

    /**
     * @brief Reassemble access units from an arbitrarily fragmented stream.
     */
    class AuSplitter {
    public:
      void feed(const std::uint8_t *data, std::size_t len, const std::function<void(std::vector<std::uint8_t> &&, bool)> &emit) {
        buffer_.insert(buffer_.end(), data, data + len);

        std::size_t pos = 0;
        for (;;) {
          auto first = find_nal(buffer_, pos, kNalAud);
          if (first == kNpos) {
            break;
          }
          auto second = find_nal(buffer_, first + 4, kNalAud);
          if (second == kNpos) {
            pos = first;
            break;
          }
          emit_slice(first, second, emit);
          pos = second;
        }

        if (pos > 0) {
          buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
        }
        if (buffer_.size() > kMaxAccessUnit) {
          buffer_.erase(buffer_.begin(), buffer_.end() - static_cast<std::ptrdiff_t>(kMaxAccessUnit));
        }
      }

    private:
      void emit_slice(std::size_t from, std::size_t to, const std::function<void(std::vector<std::uint8_t> &&, bool)> &emit) {
        std::vector<std::uint8_t> au(buffer_.begin() + static_cast<std::ptrdiff_t>(from), buffer_.begin() + static_cast<std::ptrdiff_t>(to));

        bool keyframe = find_nal(au, 0, kNalIdr) != kNpos;

        // Sunshine hands three byte start codes to the packetizer.
        if (au.size() > 4 && au[0] == 0x00 && au[1] == 0x00 && au[2] == 0x00 && au[3] == 0x01) {
          au.erase(au.begin());
        }

        emit(std::move(au), keyframe);
      }

      std::vector<std::uint8_t> buffer_;
    };

    /**
     * @brief Decode an HTTP/1.1 chunked body.
     */
    class ChunkedDecoder {
    public:
      void push(const std::uint8_t *data, std::size_t len) {
        inbox_.insert(inbox_.end(), data, data + len);
      }

      /**
       * @brief Move every complete payload byte into out.
       *
       * @return False when the body ended or the framing is broken.
       */
      bool drain(std::vector<std::uint8_t> &out) {
        for (;;) {
          if (finished_) {
            return false;
          }
          if (remaining_ == 0) {
            std::size_t eol = kNpos;
            for (std::size_t i = 0; i + 1 < inbox_.size(); ++i) {
              if (inbox_[i] == '\r' && inbox_[i + 1] == '\n') {
                eol = i;
                break;
              }
            }
            if (eol == kNpos) {
              return true;
            }

            std::size_t size = 0;
            for (std::size_t i = 0; i < eol; ++i) {
              const auto c = static_cast<char>(inbox_[i]);
              int digit = -1;
              if (c >= '0' && c <= '9') {
                digit = c - '0';
              }
              else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
              }
              else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
              }
              if (digit < 0) {
                finished_ = true;
                return false;
              }
              size = size * 16 + static_cast<std::size_t>(digit);
            }
            inbox_.erase(inbox_.begin(), inbox_.begin() + static_cast<std::ptrdiff_t>(eol + 2));
            if (size == 0) {
              finished_ = true;
              return false;
            }
            remaining_ = size;
          }

          if (inbox_.size() < remaining_ + 2) {
            return true;
          }
          out.insert(out.end(), inbox_.begin(), inbox_.begin() + static_cast<std::ptrdiff_t>(remaining_));
          inbox_.erase(inbox_.begin(), inbox_.begin() + static_cast<std::ptrdiff_t>(remaining_ + 2));
          remaining_ = 0;
        }
      }

    private:
      std::vector<std::uint8_t> inbox_;
      std::size_t remaining_ {0};
      bool finished_ {false};
    };

    std::string env_socket() {
      const char *value = std::getenv("SUNSHINE_LGMD_SOCKET");
      return value ? std::string {value} : std::string {};
    }

    int connect_unix(const std::string &path, std::string &err) {
      if (path.empty()) {
        err = "SUNSHINE_LGMD_SOCKET is empty";
        return -1;
      }
      if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        err = "socket path is too long";
        return -1;
      }

      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) {
        err = std::strerror(errno);
        return -1;
      }

      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, path.c_str(), path.size());

      if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        err = std::strerror(errno);
        ::close(fd);
        return -1;
      }
      return fd;
    }

    /**
     * @brief Read with a timeout.
     *
     * @return Bytes read, 0 on timeout, -1 on error or end of stream.
     */
    ssize_t read_some(int fd, std::uint8_t *dst, std::size_t len, int timeout_ms) {
      pollfd pfd {fd, POLLIN, 0};
      const int rc = ::poll(&pfd, 1, timeout_ms);
      if (rc == 0) {
        return 0;
      }
      if (rc < 0) {
        return -1;
      }
      const ssize_t n = ::recv(fd, dst, len, 0);
      return n > 0 ? n : -1;
    }

    bool read_head(int fd, std::string &head, std::vector<std::uint8_t> &leftover, std::string &err) {
      std::string buffer;
      std::uint8_t chunk[4096];

      for (;;) {
        const auto pos = buffer.find("\r\n\r\n");
        if (pos != std::string::npos) {
          head = buffer.substr(0, pos);
          leftover.assign(buffer.begin() + static_cast<std::ptrdiff_t>(pos + 4), buffer.end());
          return true;
        }

        const auto n = read_some(fd, chunk, sizeof(chunk), 5000);
        if (n <= 0) {
          err = "no response head from the capture daemon";
          return false;
        }
        buffer.append(reinterpret_cast<char *>(chunk), static_cast<std::size_t>(n));
        if (buffer.size() > 64 * 1024) {
          err = "response head is too large";
          return false;
        }
      }
    }

  }  // namespace

  bool enabled() {
    return !env_socket().empty();
  }

  std::string socket_path() {
    return env_socket();
  }

  void run(safe::mail_t mail, video::config_t config, stream::session_t *channel_data) {
    (void) channel_data;

    auto shutdown = mail->event<bool>(mail::shutdown);
    auto packets = mail->queue<video::packet_t>(mail::video_packets);

    auto touch_port_events = mail->event<input::touch_port_t>(mail::touch_port);
    auto hdr_events = mail->event<video::hdr_info_t>(mail::hdr);

    // Absolute mouse input needs the geometry of the streamed surface. The
    // proxy source always delivers the requested size, so the mapping is 1:1.
    input::touch_port_t port {};
    port.offset_x = 0;
    port.offset_y = 0;
    port.width = config.width;
    port.height = config.height;
    port.logical_width = config.width;
    port.logical_height = config.height;
    port.env_offset_x = 0;
    port.env_offset_y = 0;
    port.env_width = config.width;
    port.env_height = config.height;
    port.client_offsetX = 0.f;
    port.client_offsetY = 0.f;
    port.scalar_inv = 1.f;
    port.scalar_tpcoords = 1.f;
    port.env_logical_width = 0;
    port.env_logical_height = 0;
    touch_port_events->raise(port);

    hdr_events->raise(std::make_unique<video::hdr_info_raw_t>(false));

    const auto path = socket_path();
    BOOST_LOG(info) << "Streaming video from "sv << path;

    std::string err;
    const int fd = connect_unix(path, err);
    if (fd < 0) {
      BOOST_LOG(error) << "Cannot reach the capture daemon at "sv << path << ": "sv << err;
      return;
    }

    const char *request = "GET /stream.h264 HTTP/1.1\r\nHost: lgmd\r\nConnection: close\r\n\r\n";
    if (::send(fd, request, std::strlen(request), MSG_NOSIGNAL) < 0) {
      BOOST_LOG(error) << "Cannot send the stream request: "sv << std::strerror(errno);
      ::close(fd);
      return;
    }

    std::string head;
    std::vector<std::uint8_t> leftover;
    if (!read_head(fd, head, leftover, err)) {
      BOOST_LOG(error) << "Capture daemon: "sv << err;
      ::close(fd);
      return;
    }
    if (head.find(" 200 ") == std::string::npos) {
      BOOST_LOG(error) << "Capture daemon rejected the request: "sv << head.substr(0, 80);
      ::close(fd);
      return;
    }
    if (head.find("chunked") == std::string::npos) {
      BOOST_LOG(error) << "Capture daemon did not send a chunked body"sv;
      ::close(fd);
      return;
    }

    ChunkedDecoder decoder;
    AuSplitter splitter;
    if (!leftover.empty()) {
      decoder.push(leftover.data(), leftover.size());
    }

    int64_t frame_nr = 1;
    std::uint64_t keyframes = 0;

    auto publish = [&](std::vector<std::uint8_t> &&au, bool keyframe) {
      auto packet = std::make_unique<video::packet_raw_avcodec>();
      if (!packet->av_packet || av_new_packet(packet->av_packet, static_cast<int>(au.size())) < 0) {
        return;
      }
      std::memcpy(packet->av_packet->data, au.data(), au.size());
      packet->av_packet->pts = frame_nr++;
      if (keyframe) {
        packet->av_packet->flags |= AV_PKT_FLAG_KEY;
        ++keyframes;
      }
      packets->raise(std::move(packet));
    };

    std::vector<std::uint8_t> body;
    std::uint8_t chunk[kReadBuffer];

    while (!shutdown->peek()) {
      const auto n = read_some(fd, chunk, sizeof(chunk), 200);
      if (n == 0) {
        continue;
      }
      if (n < 0) {
        BOOST_LOG(info) << "Capture daemon closed the stream after "sv << (frame_nr - 1) << " frames"sv;
        break;
      }

      decoder.push(chunk, static_cast<std::size_t>(n));
      body.clear();
      if (!decoder.drain(body)) {
        BOOST_LOG(info) << "Capture daemon finished the stream after "sv << (frame_nr - 1) << " frames"sv;
        break;
      }
      if (!body.empty()) {
        splitter.feed(body.data(), body.size(), publish);
      }
    }

    ::close(fd);
    BOOST_LOG(info) << "Proxy video source stopped after "sv << (frame_nr - 1) << " frames ("sv << keyframes << " keyframes)"sv;
  }

}  // namespace proxy
