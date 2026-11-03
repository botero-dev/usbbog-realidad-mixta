class_name CameraSourceUdp
extends CameraSource

## Receives the brain firmware's low-latency video as UDP datagrams.
##
## The board sends the newest JPEG only, split into self-describing fragments on
## port 82 (see firmware/brain/src/video_udp.cpp). It streams to whoever sends
## "BV1 SUB" to that port and stops two seconds after the last subscribe packet,
## so this class repeats the packet every KEEPALIVE_MS.
##
## Loss policy: a frame with a missing fragment is discarded, never partially
## decoded. `loss_ratio` is the cumulative ratio of lost fragments to expected
## fragments, and `frames_dropped` counts incomplete frames plus frames
## superseded before they could be decoded.

const MAGIC := 0x4256
const VERSION := 1
const TYPE_VIDEO := 1
const HEADER_BYTES := 20
const MAX_FRAGMENTS := 1024
const MAX_FRAME_BYTES := 4 * 1024 * 1024
const SUBSCRIBE_PACKET := "BV1 SUB"
const KEEPALIVE_MS := 500
const INCOMPLETE_TIMEOUT_MS := 500

@export var host := "192.168.4.1"
@export var port := 82
@export var local_port := 8767
## Keep subscribing after a stall. UDP has no connection to re-establish.
@export var auto_reconnect := true
## Report the feed as closed after this long without a complete frame; 0 disables.
@export var stall_timeout_seconds := 2.0

var _udp: PacketPeerUDP = null
var _running := false
var _opened_reported := false
var _stalled := false
var _last_keepalive_ms := 0
var _last_frame_ms := 0

var _frame_id := -1
var _ignored_frame_id := -1
var _frag_count := 0
var _total_len := 0
var _parts: Array[PackedByteArray] = []
var _received := 0
var _frame_started_ms := 0

var _pending_jpeg := PackedByteArray()
var _texture: ImageTexture = null
var _frames := 0
var _fps_mark_ms := 0

var _fragments_received := 0
var _fragments_expected := 0
var _fragments_lost := 0


func _ready() -> void:
	set_process(false)


func start() -> void:
	stop()
	fps = 0.0
	frames_dropped = 0
	loss_ratio = 0.0
	_frames = 0
	_fragments_received = 0
	_fragments_expected = 0
	_fragments_lost = 0
	_opened_reported = false
	_stalled = false
	_ignored_frame_id = -1

	_udp = PacketPeerUDP.new()
	var error := _udp.bind(local_port, "*", 1 << 20)
	if error != OK:
		_udp = null
		stream_closed.emit(
			"could not bind UDP port %d (error %d)" % [local_port, error]
		)
		return

	_running = true
	_last_frame_ms = Time.get_ticks_msec()
	_fps_mark_ms = _last_frame_ms
	_send_subscribe()
	set_process(true)


func stop() -> void:
	_running = false
	if _udp != null:
		_udp.close()
		_udp = null
	_pending_jpeg = PackedByteArray()
	_reset_frame()
	set_process(false)


func is_streaming() -> bool:
	return _running and _opened_reported and not _stalled


func target() -> String:
	return host


func _process(_delta: float) -> void:
	if not _running:
		return

	var now := Time.get_ticks_msec()
	if now - _last_keepalive_ms >= KEEPALIVE_MS:
		_send_subscribe()

	_drain(now)
	_expire_incomplete(now)
	_check_stall(now)
	if _running:
		_emit_latest_frame()


func _send_subscribe() -> void:
	if _udp == null:
		return
	if _udp.set_dest_address(host, port) == OK:
		_udp.put_packet(SUBSCRIBE_PACKET.to_utf8_buffer())
	_last_keepalive_ms = Time.get_ticks_msec()


func _drain(now: int) -> void:
	while _udp != null and _udp.get_available_packet_count() > 0:
		var packet := _udp.get_packet()
		if packet.size() >= HEADER_BYTES:
			_handle_packet(packet, now)


func _handle_packet(packet: PackedByteArray, now: int) -> void:
	if packet.decode_u16(0) != MAGIC:
		return
	if packet.decode_u8(2) != VERSION or packet.decode_u8(3) != TYPE_VIDEO:
		return

	var frame_id := packet.decode_u32(4)
	var total_len := packet.decode_u32(8)
	var frag_index := packet.decode_u16(12)
	var frag_count := packet.decode_u16(14)
	var payload_len := packet.decode_u16(16)

	if frag_count == 0 or frag_count > MAX_FRAGMENTS:
		return
	if frag_index >= frag_count:
		return
	if total_len == 0 or total_len > MAX_FRAME_BYTES:
		return
	if payload_len == 0 or HEADER_BYTES + payload_len > packet.size():
		return
	if frame_id == _ignored_frame_id:
		return

	if frame_id != _frame_id:
		if _frag_count > 0 and _received < _frag_count:
			_drop_incomplete_frame()
		# A burst of late fragments from the frame just dropped must not start
		# a new, permanently incomplete one.
		if frame_id == _ignored_frame_id:
			return
		_begin_frame(frame_id, frag_count, total_len, now)
	elif _frag_count == 0:
		return  # this frame already completed

	if _received >= _frag_count:
		return
	if not _parts[frag_index].is_empty():
		return  # duplicate fragment

	_parts[frag_index] = packet.slice(HEADER_BYTES, HEADER_BYTES + payload_len)
	_received += 1
	_fragments_received += 1

	if _received == _frag_count:
		_finish_frame()


func _begin_frame(frame_id: int, frag_count: int, total_len: int, now: int) -> void:
	_frame_id = frame_id
	_frag_count = frag_count
	_total_len = total_len
	_received = 0
	_parts.clear()
	_parts.resize(frag_count)
	_frame_started_ms = now


func _finish_frame() -> void:
	var jpeg := PackedByteArray()
	for part in _parts:
		jpeg.append_array(part)

	_fragments_expected += _frag_count
	_update_loss_ratio()
	if jpeg.size() != _total_len:
		frames_dropped += 1
		_ignored_frame_id = _frame_id
		_reset_frame()
		return

	if not _pending_jpeg.is_empty():
		frames_dropped += 1
	_pending_jpeg = jpeg
	_last_frame_ms = Time.get_ticks_msec()
	_stalled = false

	if not _opened_reported:
		_opened_reported = true
		stream_opened.emit()

	# Keep _frame_id so late duplicates of this frame are ignored.
	_parts.clear()
	_frag_count = 0
	_received = 0
	_frame_started_ms = 0


func _drop_incomplete_frame() -> void:
	if _frag_count <= 0:
		return
	_fragments_expected += _frag_count
	var missing := _frag_count - _received
	if missing > 0:
		_fragments_lost += missing
	frames_dropped += 1
	_ignored_frame_id = _frame_id
	_update_loss_ratio()
	_reset_frame()


func _expire_incomplete(now: int) -> void:
	if _frag_count > 0 and now - _frame_started_ms > INCOMPLETE_TIMEOUT_MS:
		_drop_incomplete_frame()


func _reset_frame() -> void:
	_frame_id = -1
	_frag_count = 0
	_total_len = 0
	_received = 0
	_parts.clear()
	_frame_started_ms = 0


func _check_stall(now: int) -> void:
	if stall_timeout_seconds <= 0.0 or _stalled:
		return
	var silent_ms := now - _last_frame_ms
	if silent_ms <= int(stall_timeout_seconds * 1000.0):
		return

	_stalled = true
	_opened_reported = false
	stream_closed.emit("no video for %.1f s" % (silent_ms / 1000.0))
	if not auto_reconnect:
		stop()


func _emit_latest_frame() -> void:
	if _pending_jpeg.is_empty():
		return
	var jpeg := _pending_jpeg
	_pending_jpeg = PackedByteArray()

	var image := Image.new()
	var error := image.load_jpg_from_buffer(jpeg)
	if error != OK or image.is_empty():
		push_warning("CameraSourceUdp: could not decode a JPEG frame")
		return

	if (
		_texture == null
		or _texture.get_width() != image.get_width()
		or _texture.get_height() != image.get_height()
		or _texture.get_format() != image.get_format()
	):
		_texture = ImageTexture.create_from_image(image)
	else:
		_texture.update(image)

	_frames += 1
	var now := Time.get_ticks_msec()
	var elapsed := now - _fps_mark_ms
	if elapsed >= 1000:
		fps = _frames * 1000.0 / float(elapsed)
		_frames = 0
		_fps_mark_ms = now
		print(
			"[udp] %.1f fps  dropped %d  loss %.2f%% (%d/%d fragments)"
			% [
				fps,
				frames_dropped,
				loss_ratio * 100.0,
				_fragments_lost,
				_fragments_expected,
			]
		)

	frame_received.emit(_texture)


func _update_loss_ratio() -> void:
	loss_ratio = (
		0.0
		if _fragments_expected == 0
		else float(_fragments_lost) / float(_fragments_expected)
	)
