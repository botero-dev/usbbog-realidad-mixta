// video_udp.h — the car's low-latency UDP link: JPEG frames and IMU samples.
// ---------------------------------------------------------------------------
// Why this exists next to /stream: TCP is a queue. When the client is slower
// than the camera, old frames pile up and latency grows without bound. UDP can
// simply drop frames, and this sender is built so that dropping is the normal
// case: it always sends the newest frame and never waits for a slow reader.
//
// The same socket and the same subscription also carry IMU samples (`type` 2,
// 50 Hz), so a client watching the car gets its attitude without a second
// connection, a second keepalive or any HTTP traffic at all.
//
// A client subscribes by sending a small datagram ("BV1 SUB") to port 82; the
// sender streams to that address until it has been silent for two seconds.
// There is no handshake to fail and no socket to tear down.
//
// Framing is in video_udp.cpp. /stream on port 81 is untouched and still works;
// do not watch both at once, because they compete for the camera's buffers.
#pragma once

namespace video_udp {

// Binds port 82 and creates the video and IMU sender tasks. Returns false if the
// socket, the mutex serialising it, or either task could not be created; the
// reason is printed to the serial log first.
bool start();

}  // namespace video_udp
