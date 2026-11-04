/**
 * @file src/proxy.h
 * @brief Declarations for the externally encoded video source.
 */
#pragma once

// standard includes
#include <memory>
#include <string>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace stream {
  struct session_t;
}

namespace video {
  struct config_t;
}

namespace proxy {

  /**
   * @brief Report whether the proxy source is configured.
   *
   * The source is enabled by setting SUNSHINE_LGMD_SOCKET to the path of a
   * lgmd unix socket. When set, the capture and encode pipeline is replaced by
   * a reader of that socket's /stream.h264 endpoint.
   *
   * @return True when the environment selects the proxy source.
   */
  bool enabled();

  /**
   * @brief Return the configured socket path.
   *
   * @return Socket path, or an empty string when the proxy is disabled.
   */
  std::string socket_path();

  /**
   * @brief Stream pre-encoded H.264 access units until the session ends.
   *
   * The access units are published to the same packet queue the encoders use,
   * so packetization, FEC and the control channel are unchanged.
   *
   * @param mail Mailbox shared with the stream threads.
   * @param config Encoding configuration requested by the client.
   * @param channel_data Stream session that owns the mailbox.
   */
  void run(safe::mail_t mail, video::config_t config, stream::session_t *channel_data);

  /**
   * @brief Forward Opus packets from the capture daemon to the audio stream.
   *
   * @param mail Mailbox shared with the stream threads.
   * @param channel_data Stream session that owns the mailbox.
   */
  void run_audio(safe::mail_t mail, stream::session_t *channel_data);

}  // namespace proxy
