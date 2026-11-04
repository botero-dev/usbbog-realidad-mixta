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
@onready var volume_button: Button = $ControlView/ControlSounds/VolumeRow/ButtonVolume
@onready var volume_popup: PopupPanel = $ControlView/VolumePopup
@onready var volume_slider: HSlider = $ControlView/VolumePopup/Margin/Box/Slider
@onready var volume_value: Label = $ControlView/VolumePopup/Margin/Box/Value
@onready var base_off: Button = $ControlView/ControlLights/ButtonBaseOff
@onready var base_low: Button = $ControlView/ControlLights/ButtonBaseLow
@onready var base_high: Button = $ControlView/ControlLights/ButtonBaseHigh
@onready var hazard_button: Button = $ControlView/ControlLights/ButtonHazard
@onready var rainbow_button: Button = $ControlView/ControlLights/ButtonRainbow
@onready var throttle_stick: VirtualStick = $ControlView/ThrottleStick
@onready var steer_stick: VirtualStick = $ControlView/SteerStick
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
var _volume_http: HTTPRequest
var _volume_timer: Timer
# The board's level in per-mille. It is STICKY on the board, so this mirrors what the
# firmware reports rather than what was last asked for.
var _volume_milli := -1
var _volume_busy := false
var _volume_pending := -1
var _volume_dragging := false
var _polling_audio := false
var _poll_busy := false
var _poll_elapsed := 0.0
var _poll_deadline_ms := 0
var _poll_request_ms := 0
var _last_play_ms := 0
var _drive_http: HTTPRequest
# Normalized commands, each kept separately: the two sticks are independent, and one
# being released must not wipe the other's input. +1 throttle is forward, +1 steer is
# right -- both in the screen/stick convention, converted in drive_url().
var _throttle := 0.0
var _steer := 0.0
var _driving := false
var _drive_busy := false
var _drive_stop_pending := false
var _drive_next_ms := 0

## Full deflection in per-mille of the mixer. The board's own page scales its whole
## range from a slider; this is the equivalent ceiling, and it is deliberately well
## under 1000 because the car reaches full speed long before the stick does.
@export var max_drive_permille := 600
## How often a held stick is re-sent. Must stay well inside the brain's 400 ms
## control watchdog (see motor_link.h), which brakes the car by itself if commands
## stop arriving -- that is the safety net, not the normal stop path.
@export var drive_interval_ms := 100

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

	_drive_http = HTTPRequest.new()
	_drive_http.timeout = 3.0
	_drive_http.request_completed.connect(_on_drive_completed)
	add_child(_drive_http)

	_volume_http = HTTPRequest.new()
	_volume_http.timeout = 5.0
	_volume_http.request_completed.connect(_on_volume_completed)
	add_child(_volume_http)

	# Dragging a slider emits a change per pixel, and one HTTP request per pixel would
	# swamp a link that already takes ~150 ms round trip. The timer collapses a drag
	# into a handful of requests, and the release sends the final value immediately.
	_volume_timer = Timer.new()
	_volume_timer.one_shot = true
	_volume_timer.wait_time = 0.15
	_volume_timer.timeout.connect(_send_volume)
	add_child(_volume_timer)

	volume_button.pressed.connect(_on_volume_button_pressed)
	volume_slider.value_changed.connect(_on_volume_changed)
	volume_slider.drag_started.connect(_on_volume_drag_started)
	volume_slider.drag_ended.connect(_on_volume_drag_ended)
	_update_volume_button()

	throttle_stick.changed.connect(_on_throttle_changed)
	throttle_stick.released.connect(_on_throttle_changed.bind(Vector2.ZERO))
	steer_stick.changed.connect(_on_steer_changed)
	steer_stick.released.connect(_on_steer_changed.bind(Vector2.ZERO))

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

	# While a stick is off-centre the command is re-asserted on a fixed cadence: the
	# brain brakes after 400 ms without one, so a single press-and-hold would otherwise
	# stop the car a fifth of a second later. Releasing both hands brakes at once (see
	# _update_drive_state), so this is the "still holding" path, not the stop path.
	if _driving and not _drive_busy and Time.get_ticks_msec() >= _drive_next_ms:
		_send_drive()


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
	volume_popup.hide()
	# Stop before hiding: a car whose video dropped must not keep driving on the
	# last command, and the stick would otherwise look released while still held.
	_release_stick()
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
		if parsed.has("volume_milli"):
			_apply_volume(int(parsed["volume_milli"]))


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


# --- driving --------------------------------------------------------------
#
# Two sticks, because a car has two independent inputs: the LEFT one is throttle
# (vertical only) and the RIGHT one is steering (horizontal only). They are separate
# so both can be held at once -- a single stick cannot express "half throttle, gently
# left" without giving up full authority on one axis in exchange for the other.
#
# They map to the board's mixer, not to four raw duties: `th` is forward and `turn` is
# steering, and the board does the mixing (see /drive in app_httpd.cpp).
#
# The steer sign is inverted because the two conventions disagree: the stick reports
# +right (screen coordinates), while `turn` is +left. Throttle is inverted for the
# same kind of reason -- down is positive on screen, forward is negative.
#
# Stopping is EXPLICIT. The brain's watchdog would brake on its own 400 ms after the
# commands stop, but a quarter second of coasting on every release is not what "stop"
# means, so letting go of BOTH sticks sends /stop (brake) at once. Letting go of one
# while the other is held keeps driving, on the axis still held.

func _on_throttle_changed(value: Vector2) -> void:
	# The stick reports +down, which is +down on screen and reverse on the car.
	_throttle = -value.y
	_update_drive_state()


func _on_steer_changed(value: Vector2) -> void:
	_steer = value.x
	_update_drive_state()


func _update_drive_state() -> void:
	if is_zero_approx(_throttle) and is_zero_approx(_steer):
		_throttle = 0.0
		_steer = 0.0
		_stop_driving()
		return
	# Send the first command immediately instead of waiting for the next tick, so a
	# fresh touch does not feel laggy.
	if not _driving:
		_driving = true
		_drive_next_ms = 0


func _send_drive() -> void:
	_drive_next_ms = Time.get_ticks_msec() + drive_interval_ms

	_drive_busy = true
	var error := _drive_http.request(
		drive_url(_steer, _throttle), [], HTTPClient.METHOD_POST
	)
	if error != OK:
		# Do not retry in a loop: if the request cannot even be started, the link is
		# gone and the board's watchdog is already doing the right thing.
		_drive_busy = false
		_driving = false
		push_warning("could not send /drive (error %d)" % error)


## The mixer command for a pair of stick positions. Split out from the request so the
## sign conventions can be checked without a car attached -- they are the one thing
## here that is easy to get backwards and hard to notice until the car drives away.
##
## `steer` is +right and `throttle` is +forward, both normalized. The firmware's
## `turn` is +LEFT, hence the inversion on steer alone.
func drive_url(steer: float, throttle: float) -> String:
	var th := int(roundf(clampf(throttle, -1.0, 1.0) * max_drive_permille))
	var turn := int(roundf(clampf(-steer, -1.0, 1.0) * max_drive_permille))
	return "http://%s/drive?th=%d&turn=%d&twist=0" % [
		camera.target(), th, turn
	]


func _stop_driving() -> void:
	if not _driving:
		return
	_driving = false
	if _drive_busy:
		# One request in flight; the brake is sent from its completion handler.
		_drive_stop_pending = true
		return
	_send_stop()


func _send_stop() -> void:
	_drive_busy = true
	var error := _drive_http.request(
		"http://%s/stop" % camera.target(), [], HTTPClient.METHOD_POST
	)
	if error != OK:
		_drive_busy = false
		push_warning("could not send /stop (error %d)" % error)


func _on_drive_completed(
	result: int, response_code: int, _headers: PackedStringArray, _body: PackedByteArray
) -> void:
	_drive_busy = false

	if result != HTTPRequest.RESULT_SUCCESS or response_code != 200:
		push_warning("drive command failed: HTTP %d (result %d)" % [response_code, result])

	if _drive_stop_pending:
		_drive_stop_pending = false
		_send_stop()


## Returns both sticks to rest and brakes. Called when the view goes away, so a dropped
## stream can never leave a stick looking held.
func _release_stick() -> void:
	throttle_stick.reset()
	steer_stick.reset()
	_throttle = 0.0
	_steer = 0.0
	_stop_driving()


# If the window loses focus mid-drag, the release event may never arrive -- and on a
# real car "the mouse left the window" must not mean "keep driving".
func _notification(what: int) -> void:
	if what == NOTIFICATION_APPLICATION_FOCUS_OUT or what == NOTIFICATION_WM_WINDOW_FOCUS_OUT:
		_release_stick()


# --- volume ---------------------------------------------------------------
#
# The level is the ONLY volume control this car has: the DAC is fixed line level and
# the amp has fixed gain, so the firmware scales the samples (see kDefaultVolume in
# audio_player.cpp). Its boot default is deliberately quiet -- 3.7%, about -29 dBFS --
# which is why "the car makes no sound" usually means "raise the volume", not "audio
# is broken". The value is STICKY on the board and survives until it is reset, so this
# mirrors what /status reports rather than assuming the boot default.

func _on_volume_button_pressed() -> void:
	var button_rect := volume_button.get_global_rect()
	var popup_size := volume_popup.get_contents_minimum_size()
	# Under the button, right-aligned to it, so it never covers the sounds it controls.
	var position := Vector2(
		button_rect.position.x + button_rect.size.x - popup_size.x,
		button_rect.position.y + button_rect.size.y + 4.0
	)
	volume_popup.popup(Rect2i(Vector2i(position), Vector2i(popup_size)))


func _on_volume_changed(value: float) -> void:
	volume_value.text = _format_volume(int(round(value * 10.0)))
	_volume_timer.start()


func _on_volume_drag_ended(value_changed: bool) -> void:
	_volume_dragging = false
	if value_changed:
		_volume_timer.stop()
		_send_volume()


func _send_volume() -> void:
	var milli := int(round(volume_slider.value * 10.0))
	# Requests never overlap; the newest value is sent when the board answers. That
	# keeps a drag from queueing a dozen requests the board has to work through.
	if _volume_busy:
		_volume_pending = milli
		return
	_volume_busy = true
	var error := _volume_http.request(
		"http://%s/control?vol=%d" % [camera.target(), milli],
		[],
		HTTPClient.METHOD_POST
	)
	if error != OK:
		_volume_busy = false
		push_warning("could not send volume (error %d)" % error)


func _on_volume_completed(
	result: int, response_code: int, _headers: PackedStringArray, body: PackedByteArray
) -> void:
	_volume_busy = false

	if result == HTTPRequest.RESULT_SUCCESS and response_code == 200:
		var parsed: Variant = JSON.parse_string(body.get_string_from_utf8())
		# The reply is the same JSON as /status, so the level the board actually applied
		# comes back with it -- including when it clamped what was asked for.
		if parsed is Dictionary and parsed.has("volume_milli"):
			_apply_volume(int(parsed["volume_milli"]))
	else:
		push_warning("volume change failed: HTTP %d" % response_code)

	if _volume_pending >= 0:
		var next := _volume_pending
		_volume_pending = -1
		volume_slider.set_value_no_signal(float(next) / 10.0)
		_send_volume()


## Adopts a level reported by the board. Does not move the slider while the user is
## dragging it, or a reply would fight the finger.
func _apply_volume(milli: int) -> void:
	_volume_milli = clampi(milli, 0, 1000)
	if not _volume_dragging:
		volume_slider.set_value_no_signal(float(_volume_milli) / 10.0)
	volume_value.text = _format_volume(_volume_milli)
	_update_volume_button()


func _on_volume_drag_started() -> void:
	_volume_dragging = true


func _update_volume_button() -> void:
	volume_button.text = (
		"Volume: ?" if _volume_milli < 0 else "Volume: " + _format_volume(_volume_milli)
	)


## Per-mille to a short percentage: the difference between 3.7% and 4% does not matter
## to a person, and "4%" is what they will compare against the slider.
func _format_volume(milli: int) -> String:
	return "%d%%" % int(round(float(milli) / 10.0))
