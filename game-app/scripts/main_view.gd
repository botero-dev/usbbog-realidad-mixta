extends Control

@onready var connect_view: Control = $ConnectView
@onready var control_view: Control = $ControlView
@onready var connect_button: Button = $ConnectView/Button
@onready var status_label: Label = $ConnectView/StatusLabel
@onready var video: TextureRect = $ControlView/Video
@onready var resolution_button: Button = $ControlView/ResolutionButton
@onready var stats_label: Label = $ControlView/StatsLabel
@onready var camera: CameraSource = $CameraFeed

var _got_frame := false
var _control_http: HTTPRequest
var _status_http: HTTPRequest
var _resolution := "vga"
var _pending_resolution := ""
var _status_busy := false
var _stats_elapsed := 0.0


func _ready() -> void:
	control_view.hide()
	connect_view.show()
	status_label.text = ""

	camera.frame_received.connect(_on_frame_received)
	camera.stream_opened.connect(_on_stream_opened)
	camera.stream_closed.connect(_on_stream_closed)

	_control_http = HTTPRequest.new()
	_control_http.timeout = 5.0
	_control_http.request_completed.connect(_on_control_completed)
	add_child(_control_http)

	_status_http = HTTPRequest.new()
	_status_http.timeout = 5.0
	_status_http.request_completed.connect(_on_status_completed)
	add_child(_status_http)

	resolution_button.pressed.connect(_on_resolution_pressed)
	_set_resolution(_resolution)


func _process(delta: float) -> void:
	if not control_view.visible:
		return
	_stats_elapsed += delta
	if _stats_elapsed < 1.0:
		return
	_stats_elapsed = 0.0
	stats_label.text = "%d fps · %d dropped · %.1f%% loss" % [
		roundi(camera.fps),
		camera.frames_dropped,
		camera.loss_ratio * 100.0,
	]


func _on_button_pressed() -> void:
	_got_frame = false
	connect_button.disabled = true
	connect_button.text = "Connecting to %s…" % camera.target()
	status_label.text = ""
	camera.start()


func _on_stream_opened() -> void:
	status_label.text = "Waiting for the first frame…"
	_refresh_resolution()


# Switch to the control view on the first real frame, not on connect: the
# placeholder icon stays up while the stream is still being negotiated.
func _on_frame_received(texture: Texture2D) -> void:
	video.texture = texture
	if _got_frame:
		return
	_got_frame = true
	connect_view.hide()
	control_view.show()
	_stats_elapsed = 1.0  # fill the stats label on the first visible frame


func _on_stream_closed(reason: String) -> void:
	_got_frame = false
	control_view.hide()
	connect_view.show()
	connect_button.disabled = false
	connect_button.text = "Reconnect to %s" % camera.target()
	status_label.text = reason


# The board boots at VGA and remembers the sensor setting until reset, so the
# button is synced from /status rather than guessed.
func _refresh_resolution() -> void:
	if _status_busy:
		return
	_status_busy = true
	var error := _status_http.request("http://%s/status" % camera.target())
	if error != OK:
		_status_busy = false


func _on_status_completed(
	result: int, response_code: int, _headers: PackedStringArray, body: PackedByteArray
) -> void:
	_status_busy = false
	if result != HTTPRequest.RESULT_SUCCESS or response_code != 200:
		return
	var parsed: Variant = JSON.parse_string(body.get_string_from_utf8())
	if parsed is Dictionary and parsed.has("framesize"):
		_set_resolution(str(parsed["framesize"]).to_lower())


func _on_resolution_pressed() -> void:
	var next := "qvga" if _resolution == "vga" else "vga"
	var error := _control_http.request(
		"http://%s/control?framesize=%s" % [camera.target(), next]
	)
	if error != OK:
		push_warning("could not request %s (error %d)" % [next, error])
		return
	_pending_resolution = next
	resolution_button.disabled = true


func _on_control_completed(
	result: int, response_code: int, _headers: PackedStringArray, body: PackedByteArray
) -> void:
	if _pending_resolution.is_empty():
		return

	var requested := _pending_resolution
	_pending_resolution = ""
	resolution_button.disabled = false

	if result == HTTPRequest.RESULT_SUCCESS and response_code == 200:
		_set_resolution(requested)
		return

	stats_label.text = "Could not switch to %s" % requested.to_upper()
	push_warning(
		"resolution change to %s failed: HTTP %d %s"
		% [requested, response_code, body.get_string_from_utf8().strip_edges()]
	)


func _set_resolution(name: String) -> void:
	_resolution = name
	resolution_button.text = "Resolution: " + _resolution.to_upper()
