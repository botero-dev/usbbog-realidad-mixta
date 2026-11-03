extends Control

@onready var connect_view: Control = $ConnectView
@onready var control_view: Control = $ControlView
@onready var connect_button: Button = $ConnectView/Button
@onready var status_label: Label = $ConnectView/StatusLabel
@onready var video: TextureRect = $ControlView/Video
@onready var resolution_button: Button = $ControlView/ResolutionButton
@onready var stats_label: Label = $ControlView/StatsLabel
@onready var base_off: Button = $ControlView/ControlLights/ButtonBaseOff
@onready var base_low: Button = $ControlView/ControlLights/ButtonBaseLow
@onready var base_high: Button = $ControlView/ControlLights/ButtonBaseHigh
@onready var hazard_button: Button = $ControlView/ControlLights/ButtonHazard
@onready var rainbow_button: Button = $ControlView/ControlLights/ButtonRainbow
@onready var camera: CameraSource = $CameraFeed

var _got_frame := false
var _control_http: HTTPRequest
var _status_http: HTTPRequest
var _led_http: HTTPRequest
var _resolution := "vga"
var _pending_resolution := ""
var _status_busy := false
var _stats_elapsed := 0.0
var _grade_material: Material
var _led_busy := false
var _led_pending := ""
# Set while pushing the board's state into the buttons: assigning button_pressed
# emits toggled, and that must not be sent back to the board as a new command.
var _applying_led_state := false


func _ready() -> void:
	control_view.hide()
	connect_view.show()
	status_label.text = ""

	# The grade is for camera pixels; the placeholder icon is already designed
	# colour, so keep it un graded until a real frame arrives.
	_grade_material = video.material
	video.material = null

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

	_led_http = HTTPRequest.new()
	_led_http.timeout = 5.0
	_led_http.request_completed.connect(_on_led_completed)
	add_child(_led_http)

	resolution_button.pressed.connect(_on_resolution_pressed)
	_set_resolution(_resolution)

	base_off.pressed.connect(func(): _request_led("base=off"))
	base_low.pressed.connect(func(): _request_led("base=low"))
	base_high.pressed.connect(func(): _request_led("base=high"))
	hazard_button.toggled.connect(_on_hazard_toggled)
	rainbow_button.toggled.connect(_on_rainbow_toggled)


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
	video.material = _grade_material
	connect_view.hide()
	control_view.show()
	_stats_elapsed = 1.0  # fill the stats label on the first visible frame


func _on_stream_closed(reason: String) -> void:
	_got_frame = false
	video.material = null
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
	if parsed is Dictionary:
		if parsed.has("framesize"):
			_set_resolution(str(parsed["framesize"]).to_lower())
		_apply_led_state(parsed)


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


# --- lights ---------------------------------------------------------------
#
# The board owns the light state, so every request's reply is the truth: /control
# answers with the same JSON as /status, and the buttons are set from THAT rather
# than from what was asked for. A request the sensor or the firmware refused then
# simply leaves the button showing the real state instead of a lie.

func _on_hazard_toggled(pressed: bool) -> void:
	if _applying_led_state:
		return
	_request_led("hazard=%d" % (1 if pressed else 0))


func _on_rainbow_toggled(pressed: bool) -> void:
	if _applying_led_state:
		return
	_request_led("rainbow=%d" % (1 if pressed else 0))


func _request_led(query: String) -> void:
	# Requests never overlap: the newest one is sent when the board answers. Rapid
	# clicking therefore settles on the last click instead of racing.
	if _led_busy:
		_led_pending = query
		return
	_led_busy = true
	var error := _led_http.request(
		"http://%s/control?%s" % [camera.target(), query]
	)
	if error != OK:
		_led_busy = false
		push_warning("could not send %s (error %d)" % [query, error])


func _on_led_completed(
	result: int, response_code: int, _headers: PackedStringArray, body: PackedByteArray
) -> void:
	_led_busy = false

	if result == HTTPRequest.RESULT_SUCCESS and response_code == 200:
		var parsed: Variant = JSON.parse_string(body.get_string_from_utf8())
		if parsed is Dictionary:
			_apply_led_state(parsed)
	else:
		push_warning(
			"light command failed: HTTP %d %s"
			% [response_code, body.get_string_from_utf8().strip_edges()]
		)

	if not _led_pending.is_empty():
		var next := _led_pending
		_led_pending = ""
		_request_led(next)


func _apply_led_state(state: Dictionary) -> void:
	_applying_led_state = true
	if state.has("led_base"):
		var base := str(state["led_base"])
		# "manual" is not reachable from this UI, but /control?led_r=.. can put the
		# board there; then none of the three is the truth, and all stay unpressed.
		base_off.button_pressed = base == "off"
		base_low.button_pressed = base == "low"
		base_high.button_pressed = base == "high"
	if state.has("led_hazard"):
		hazard_button.button_pressed = int(state["led_hazard"]) != 0
	if state.has("led_rainbow"):
		rainbow_button.button_pressed = int(state["led_rainbow"]) != 0
	_applying_led_state = false
