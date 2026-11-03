extends Control

@onready var connect_view: Control = $ConnectView
@onready var control_view: Control = $ControlView
@onready var connect_button: Button = $ConnectView/Button
@onready var status_label: Label = $ConnectView/StatusLabel
@onready var video: TextureRect = $ControlView/Video
@onready var resolution_button: Button = $ControlView/ResolutionButton
@onready var stats_label: Label = $ControlView/StatsLabel
@onready var clip_buttons: HFlowContainer = $ControlView/ControlSounds/ClipButtons
@onready var clip_status: Label = $ControlView/ControlSounds/ClipStatus
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
var _play_http: HTTPRequest
var _poll_http: HTTPRequest
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
# The sound sources the buttons currently represent, in board order. Rebuilding
# them on every reply would fight with a click in flight, so only a change does.
var _clip_names: Array[String] = []
var _play_busy := false
var _play_pending := ""
var _polling_audio := false
var _poll_busy := false
var _poll_elapsed := 0.0
var _poll_deadline_ms := 0
var _poll_request_ms := 0
var _last_play_ms := 0

## `src=beeps` is the firmware's test tone. It is not in `clips[]` -- it is not an
## embedded clip -- but /play accepts it always, so it gets a button too.
const BEEPS_SRC := "beeps"
const AUDIO_POLL_INTERVAL_S := 0.75
const AUDIO_POLL_LIMIT_MS := 20000


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

	_play_http = HTTPRequest.new()
	_play_http.timeout = 5.0
	_play_http.request_completed.connect(_on_play_completed)
	add_child(_play_http)

	_poll_http = HTTPRequest.new()
	_poll_http.timeout = 5.0
	_poll_http.request_completed.connect(_on_poll_completed)
	add_child(_poll_http)

	resolution_button.pressed.connect(_on_resolution_pressed)
	_set_resolution(_resolution)

	# The board's clip table arrives with the first /status, but beeps is always
	# available, so the panel is never empty if that request fails.
	_populate_clips([])

	base_off.pressed.connect(func(): _request_led("base=off"))
	base_low.pressed.connect(func(): _request_led("base=low"))
	base_high.pressed.connect(func(): _request_led("base=high"))
	hazard_button.toggled.connect(_on_hazard_toggled)
	rainbow_button.toggled.connect(_on_rainbow_toggled)


func _process(delta: float) -> void:
	if not control_view.visible:
		return

	_stats_elapsed += delta
	if _stats_elapsed >= 1.0:
		_stats_elapsed = 0.0
		stats_label.text = "%d fps · %d dropped · %.1f%% loss" % [
			roundi(camera.fps),
			camera.frames_dropped,
			camera.loss_ratio * 100.0,
		]

	# "now playing" is only meaningful while something is playing, so the board is
	# polled for it until it reports idle -- rather than leaving a clip name on screen
	# long after the sound stopped.
	if _polling_audio:
		_poll_elapsed += delta
		if Time.get_ticks_msec() > _poll_deadline_ms:
			_polling_audio = false
			clip_status.text = ""
		elif _poll_elapsed >= AUDIO_POLL_INTERVAL_S and not _poll_busy:
			_poll_elapsed = 0.0
			_poll_busy = true
			_poll_request_ms = Time.get_ticks_msec()
			if _poll_http.request("http://%s/status" % camera.target()) != OK:
				_poll_busy = false
				_polling_audio = false


func _on_button_pressed() -> void:
	_got_frame = false
	connect_button.disabled = true
	connect_button.text = "Connecting to %s…" % camera.target()
	status_label.text = ""
	camera.start()


func _on_stream_opened() -> void:
	status_label.text = "Waiting for the first frame…"
	_refresh_board_state()


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
	_polling_audio = false
	clip_status.text = ""
	control_view.hide()
	connect_view.show()
	connect_button.disabled = false
	connect_button.text = "Reconnect to %s" % camera.target()
	status_label.text = reason


# The board owns its own configuration -- the sensor setting, the lights and the
# clip table all survive as long as it is powered -- so everything shown here is read
# from /status rather than guessed or hardcoded.
func _refresh_board_state() -> void:
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
		if parsed.has("clips"):
			_populate_clips(parsed["clips"])
		if parsed.has("audio"):
			_update_audio_label(str(parsed["audio"]))


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


# --- sounds ---------------------------------------------------------------
#
# The buttons are built from the board's OWN clip table (`clips[]` in /status), so
# adding a clip to the firmware needs no change here -- the same rule the board's
# own web page follows.

func _populate_clips(clips: Array) -> void:
	var names: Array[String] = [BEEPS_SRC]
	for clip in clips:
		names.append(str(clip))

	# Rebuilding on every reply would free the button under the cursor, so only a
	# change in the list does anything.
	if names == _clip_names:
		return
	_clip_names = names

	for child in clip_buttons.get_children():
		clip_buttons.remove_child(child)
		child.queue_free()

	for name in names:
		var button := Button.new()
		button.text = _clip_label(name)
		# The project theme's default button font is 40 px, sized for the big
		# connect button. Eight clip buttons at that size would cover the video.
		button.add_theme_font_size_override("font_size", 20)
		button.pressed.connect(_on_clip_pressed.bind(name))
		clip_buttons.add_child(button)


## Turns a firmware slug into something readable without inventing a name the board
## does not know: "kachao1" -> "Kachao 1", "rapido" -> "Rapido".
func _clip_label(name: String) -> String:
	if name == BEEPS_SRC:
		return "Beeps"

	var out := ""
	for i in name.length():
		var c := name[i]
		if i > 0 and _is_digit(c) and not _is_digit(name[i - 1]):
			out += " "
		out += c
	return out.substr(0, 1).to_upper() + out.substr(1)


func _is_digit(c: String) -> bool:
	return c >= "0" and c <= "9"


func _on_clip_pressed(src: String) -> void:
	# Requests never overlap: the newest click is sent when the board answers, so a
	# burst of presses ends on the last one instead of queuing up a racket.
	if _play_busy:
		_play_pending = src
		return
	_play_busy = true
	var error := _play_http.request(
		"http://%s/play?src=%s" % [camera.target(), src.uri_encode()]
	)
	if error != OK:
		_play_busy = false
		clip_status.text = "could not reach the board"
		push_warning("could not send /play?src=%s (error %d)" % [src, error])
		return

	# Show it now rather than when the reply arrives: the board starts the clip on
	# receipt, so waiting for the round trip would make the button feel broken. A
	# refusal replaces this with the board's error.
	#
	# Polling is NOT started here: a poll sent now could be answered before the
	# board has reached /play, and seeing "idle" would blank the label we just set.
	# The reply starts it, by which time the clip is definitely running.
	_last_play_ms = Time.get_ticks_msec()
	_poll_elapsed = 0.0
	_update_audio_label(src)


func _on_play_completed(
	result: int, response_code: int, _headers: PackedStringArray, body: PackedByteArray
) -> void:
	_play_busy = false

	if result == HTTPRequest.RESULT_SUCCESS and response_code == 200:
		var parsed: Variant = JSON.parse_string(body.get_string_from_utf8())
		if parsed is Dictionary:
			# Refresh the clip list, but NOT the audio label from this reply: the
			# board answers /play before the audio task has picked the request up,
			# so `audio` here is still "idle" and would blank the label that was set
			# on the press. The poll below reports the truth about a second later.
			if parsed.has("clips"):
				_populate_clips(parsed["clips"])
		_start_audio_poll()
	else:
		var detail := body.get_string_from_utf8().strip_edges()
		var parsed: Variant = JSON.parse_string(detail)
		if parsed is Dictionary and parsed.has("error"):
			detail = str(parsed["error"])
		clip_status.text = "could not play: %s" % detail

	if not _play_pending.is_empty():
		var next := _play_pending
		_play_pending = ""
		_on_clip_pressed(next)


func _update_audio_label(audio: String) -> void:
	if audio.is_empty() or audio == "idle":
		clip_status.text = ""
		_polling_audio = false
		return
	clip_status.text = "now playing: " + _clip_label(audio)


## The board says what it is playing but does not say when it stops, so it is asked
## until it reports idle. Bounded, because a board that stops answering must not
## leave "now playing" on screen for ever.
func _start_audio_poll() -> void:
	_polling_audio = true
	_poll_elapsed = 0.0
	# Deliberately no reset of _poll_busy: a request already in flight is still
	# asking the right question ("what is playing?"), and forgetting that it is in
	# flight would let a second one start on the same HTTPRequest, which refuses it.
	_poll_deadline_ms = Time.get_ticks_msec() + AUDIO_POLL_LIMIT_MS


func _on_poll_completed(
	result: int, response_code: int, _headers: PackedStringArray, body: PackedByteArray
) -> void:
	_poll_busy = false
	# A reply that was requested before the newest play was confirmed describes the
	# moment before it, so believing it would clear the label for a clip that just
	# started. The next poll corrects anything older than this.
	if _poll_request_ms < _last_play_ms:
		return
	if result != HTTPRequest.RESULT_SUCCESS or response_code != 200:
		_polling_audio = false
		return
	var parsed: Variant = JSON.parse_string(body.get_string_from_utf8())
	if parsed is Dictionary and parsed.has("audio"):
		_update_audio_label(str(parsed["audio"]))
