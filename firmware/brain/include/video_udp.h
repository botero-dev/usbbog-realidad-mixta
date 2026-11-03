// video_udp.h — the low-latency video path: JPEG frames as UDP datagrams.
// ---------------------------------------------------------------------------
// Why this exists next to /stream: TCP is a queue. When the client is slower
// than the camera, old frames pile up and latency grows without bound. UDP can
// simply drop frames, and this sender is built so that dropping is the normal
// case: it always sends the newest frame and never waits for a slow reader.
//
// A client subscribes by sending a small datagram ("BV1 SUB") to port 82; the
// sender streams to that address until it has been silent for two seconds.
// There is no handshake to fail and no socket to tear down.
//
// Framing is in video_udp.cpp. /stream on port 81 is untouched and still works;
// do not watch both at once, because they compete for the camera's buffers.
#pragma once

namespace video_udp {

// Creates the sender task. Returns false if the task could not be created;
// socket errors are reported from the task itself.
bool start();

}  // namespace video_udp
