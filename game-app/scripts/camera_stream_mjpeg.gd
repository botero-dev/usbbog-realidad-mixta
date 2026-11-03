class_name CameraSourceMjpeg
extends CameraSource

## Reads the brain firmware's MJPEG stream over TCP.
##
## The board serves `GET /stream` on port 81 (see
## firmware/brain/src/app_httpd.cpp): a chunked `multipart/x-mixed-replace`
## response with one JPEG per part, boundary "frame". This class de-chunks the
## HTTP body, splits it on the boundary and decodes each part into a texture.
##
## The machine running the game must be joined to the board's SoftAP, so that
## the board's address (192.168.4.1 by default) is reachable.
##
## TCP is what the firmware already serves. `CameraSource` is the seam that lets
## a UDP implementation replace this without touching the UI.

enum State { IDLE, CONNECTING, READING, RETRY }
enum ChunkState { HEADERS, SIZE, DATA, DATA_END, TRAILERS }
enum PartState { BOUNDARY, HEADERS, JPEG }

const DEFAULT_BOUNDARY := "frame"
const CONNECT_TIMEOUT_MS := 5000
const RECONNECT_DELAY_MS := 1000
const MAX_HEADER_BYTES := 8192

@export var host := "192.168.4.1"
@export var port := 81
@export var path := "/stream"
## Reopen the stream automatically after it drops.
@export var auto_reconnect := true

var _tcp: StreamPeerTCP = null
var _state := State.IDLE

var _raw := PackedByteArray()
var _chunk_state := ChunkState.HEADERS
var _chunk_left := 0

var _body := PackedByteArray()
var _part_state := PartState.BOUNDARY
var _boundary := DEFAULT_BOUNDARY
var _boundary_token := "--frame".to_utf8_buffer()
var _jpeg := PackedByteArray()
var _jpeg_left := 0

var _texture: ImageTexture = null
var _frames := 0
var _fps_mark_ms := 0

var _connect_started_ms := 0
var _retry_at_ms := 0

var _crlf := "\r\n".to_utf8_buffer()
var _crlfcrlf := "\r\n\r\n".to_utf8_buffer()


func _ready() -> void:
	set_process(false)


func start() -> void:
	stop()
	_reset_parser()
	_connect_started_ms = Time.get_ticks_msec()
	_fps_mark_ms = _connect_started_ms
	_state = State.CONNECTING
	_tcp = StreamPeerTCP.new()
	var error := _tcp.connect_to_host(host, port)
	set_process(true)
	if error != OK:
		_fail("could not reach %s:%d (error %d)" % [host, port, error])


func stop() -> void:
	_close_socket()
	_reset_parser()
	_state = State.IDLE
	set_process(false)


func is_streaming() -> bool:
	return _state == State.READING


func target() -> String:
	return host


func _process(_delta: float) -> void:
	match _state:
		State.CONNECTING:
			_poll_connect()
		State.READING:
			_poll_read()
		State.RETRY:
			if Time.get_ticks_msec() >= _retry_at_ms:
				start()


func _poll_connect() -> void:
	_tcp.poll()
	var status := _tcp.get_status()
	if status == StreamPeerTCP.STATUS_ERROR:
		_fail("connection to %s:%d failed" % [host, port])
		return
	if status != StreamPeerTCP.STATUS_CONNECTED:
		if Time.get_ticks_msec() - _connect_started_ms > CONNECT_TIMEOUT_MS:
			_fail("timed out connecting to %s:%d" % [host, port])
		return

	var request := (
		"GET %s HTTP/1.1\r\n"
		+ "Host: %s:%d\r\n"
		+ "Accept: multipart/x-mixed-replace\r\n"
		+ "Cache-Control: no-cache\r\n"
		+ "Connection: keep-alive\r\n\r\n"
	) % [path, host, port]
	var error := _tcp.put_data(request.to_utf8_buffer())
	if error != OK:
		_fail("could not send the stream request (error %d)" % error)
		return
	_state = State.READING


func _poll_read() -> void:
	_tcp.poll()
	if _tcp.get_status() != StreamPeerTCP.STATUS_CONNECTED:
		_fail("the connection to %s:%d closed" % [host, port])
		return

	while true:
		var available := _tcp.get_available_bytes()
		if available <= 0:
			break
		var result := _tcp.get_data(available)
		if result[0] != OK:
			_fail("read failed (error %d)" % result[0])
			return
		var data: PackedByteArray = result[1]
		if data.is_empty():
			break
		_raw.append_array(data)

	_consume()


## Removes HTTP chunk framing from _raw and feeds the payload to the multipart
## parser. esp_http_server sends one chunk per boundary/part header/JPEG.
func _consume() -> void:
	while true:
		match _chunk_state:
			ChunkState.HEADERS:
				var at := _find_bytes(_raw, _crlfcrlf)
				if at < 0:
					if _raw.size() > MAX_HEADER_BYTES:
						_fail("HTTP response headers are too large")
					return
				var head := _raw.slice(0, at).get_string_from_ascii()
				_raw = _raw.slice(at + _crlfcrlf.size())

				var status_line := head.get_slice("\r\n", 0)
				if not status_line.contains(" 200"):
					_fail("HTTP request failed: %s" % status_line)
					return

				_boundary = _boundary_from_headers(head)
				_boundary_token = ("--" + _boundary).to_utf8_buffer()
				_chunk_state = ChunkState.SIZE
				stream_opened.emit()

			ChunkState.SIZE:
				var at := _find_bytes(_raw, _crlf)
				if at < 0:
					return
				var line := _raw.slice(0, at).get_string_from_ascii().strip_edges()
				_raw = _raw.slice(at + _crlf.size())

				var size_text := line.get_slice(";", 0).strip_edges()
				if size_text.is_empty() or not size_text.is_valid_hex_number():
					_fail("malformed chunk size: \"%s\"" % line)
					return
				_chunk_left = size_text.hex_to_int()
				_chunk_state = (
					ChunkState.DATA if _chunk_left > 0 else ChunkState.TRAILERS
				)

			ChunkState.DATA:
				if _raw.is_empty():
					return
				var take := mini(_chunk_left, _raw.size())
				_feed_part(_raw.slice(0, take))
				_raw = _raw.slice(take)
				_chunk_left -= take
				if _chunk_left == 0:
					_chunk_state = ChunkState.DATA_END

			ChunkState.DATA_END:
				if _raw.size() < _crlf.size():
					return
				_raw = _raw.slice(_crlf.size())
				_chunk_state = ChunkState.SIZE

			ChunkState.TRAILERS:
				_fail("the board closed the stream")
				return


func _feed_part(data: PackedByteArray) -> void:
	_body.append_array(data)
	_parse_parts()


## Splits the de-chunked body into parts: "--frame", a Content-Length header,
## then exactly that many JPEG bytes.
func _parse_parts() -> void:
	while true:
		match _part_state:
			PartState.BOUNDARY:
				var at := _find_bytes(_body, _boundary_token)
				if at < 0:
					# Keep a tail that may still be the start of the token.
					var keep := _boundary_token.size() - 1
					if _body.size() > keep:
						_body = _body.slice(_body.size() - keep)
					return
				_body = _body.slice(at + _boundary_token.size())
				_part_state = PartState.HEADERS

			PartState.HEADERS:
				var at := _find_bytes(_body, _crlfcrlf)
				if at < 0:
					return
				var headers := _body.slice(0, at).get_string_from_ascii()
				_body = _body.slice(at + _crlfcrlf.size())

				var length := _content_length(headers)
				if length <= 0:
					_part_state = PartState.BOUNDARY
					continue
				_jpeg = PackedByteArray()
				_jpeg_left = length
				_part_state = PartState.JPEG

			PartState.JPEG:
				var take := mini(_jpeg_left, _body.size())
				if take > 0:
					_jpeg.append_array(_body.slice(0, take))
					_body = _body.slice(take)
					_jpeg_left -= take
				if _jpeg_left > 0:
					return
				_emit_jpeg()
				_part_state = PartState.BOUNDARY


func _emit_jpeg() -> void:
	var image := Image.new()
	var error := image.load_jpg_from_buffer(_jpeg)
	_jpeg = PackedByteArray()
	if error != OK or image.is_empty():
		push_warning("CameraSourceMjpeg: could not decode a JPEG frame")
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

	frame_received.emit(_texture)


func _fail(reason: String) -> void:
	_close_socket()
	_reset_parser()
	stream_closed.emit(reason)

	if auto_reconnect:
		_state = State.RETRY
		_retry_at_ms = Time.get_ticks_msec() + RECONNECT_DELAY_MS
		set_process(true)
	else:
		_state = State.IDLE
		set_process(false)


func _reset_parser() -> void:
	_raw = PackedByteArray()
	_body = PackedByteArray()
	_jpeg = PackedByteArray()
	_chunk_state = ChunkState.HEADERS
	_part_state = PartState.BOUNDARY
	_boundary = DEFAULT_BOUNDARY
	_boundary_token = ("--" + _boundary).to_utf8_buffer()
	_chunk_left = 0
	_jpeg_left = 0


func _close_socket() -> void:
	if _tcp != null:
		_tcp.disconnect_from_host()
		_tcp = null


func _boundary_from_headers(headers: String) -> String:
	for line in headers.split("\r\n"):
		var colon := line.find(":")
		if colon < 0:
			continue
		if line.substr(0, colon).strip_edges().to_lower() != "content-type":
			continue
		var at := line.to_lower().find("boundary=")
		if at < 0:
			continue
		var value := line.substr(at + "boundary=".length()).strip_edges()
		value = value.get_slice(";", 0).strip_edges()
		value = value.trim_prefix("\"").trim_suffix("\"")
		if not value.is_empty():
			return value
	return DEFAULT_BOUNDARY


func _content_length(headers: String) -> int:
	for line in headers.split("\r\n"):
		var colon := line.find(":")
		if colon < 0:
			continue
		if line.substr(0, colon).strip_edges().to_lower() != "content-length":
			continue
		var value := line.substr(colon + 1).strip_edges()
		if value.is_valid_int():
			return value.to_int()
	return -1


## Substring search over raw bytes; PackedByteArray.find() only matches one byte.
func _find_bytes(haystack: PackedByteArray, needle: PackedByteArray, from := 0) -> int:
	if needle.is_empty() or haystack.size() < needle.size():
		return -1
	var last := haystack.size() - needle.size()
	for i in range(maxi(from, 0), last + 1):
		var found := true
		for j in needle.size():
			if haystack[i + j] != needle[j]:
				found = false
				break
		if found:
			return i
	return -1
