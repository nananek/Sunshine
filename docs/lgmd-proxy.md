# lgmd proxy source

This branch replaces Sunshine's capture and encode stages with a reader for a
pre-encoded H.264 stream, so a capture device that already encodes video (an
HDMI capture stick) can be streamed to Moonlight without decoding and
re-encoding it on the host.

The upstream pipeline is:

```
display capture -> encoder -> packet queue -> RTP + FEC -> client
```

With `SUNSHINE_LGMD_SOCKET` set it becomes:

```
unix socket -> access units -> packet queue -> RTP + FEC -> client
```

Everything downstream of the packet queue is untouched: packetization, FEC,
the control channel and input handling are the same code paths as before.

## Usage

1. Run the capture daemon that owns the device and serves `/stream.h264`
   (lgmd) on a unix socket.
2. Start Sunshine with the socket path in the environment:

```sh
SUNSHINE_LGMD_SOCKET="$XDG_RUNTIME_DIR/lgmd.sock" ./build/sunshine
```

3. Pair and connect from Moonlight as usual.

## What the proxy does

- Opens the socket and issues `GET /stream.h264`.
- Decodes the HTTP/1.1 chunked body and splits the Annex B stream into access
  units on access unit delimiters, so buffer boundaries do not have to line up
  with frame boundaries.
- Rewrites the leading four byte start code to three bytes, matching what the
  encoders hand to the packetizer.
- Publishes one packet per access unit, flagged as a keyframe when an IDR slice
  is present.
- Raises the touch-port geometry from the requested mode, so absolute mouse
  input maps 1:1 onto the streamed surface.
- Stops when the session shuts down, or when the daemon closes the stream.

## Limitations

- **H.264 only.** The stream is whatever the device encoded, so the codec flags
  advertised to clients are restricted to H.264. HEVC and AV1 are not offered.
- **Resolution is the capture's, not the client's.** A 1080p capture is sent as
  1080p even if the client asks for 720p. Select 1920x1080@60 in Moonlight.
- **No keyframe on demand.** The device emits an IDR every 15 frames, so a
  client that requests one waits at most a quarter of a second.
- **Audio is not proxied.** Sunshine captures host audio as usual; the capture
  device's HDMI audio is not part of this stream.
- **The stream ends if the daemon restarts.** The session does not reconnect;
  the client reconnects and the daemon re-serves from the next keyframe.
