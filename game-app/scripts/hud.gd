class_name AttitudeHud
extends Control

## Artificial horizon, pitch ladder and bank scale for the car's camera feed.
##
## The overlay is placed through the camera's optics instead of by eye: a frame
## from a rectilinear (pinhole) lens projects a direction `a` off the optical axis
## `f * tan(a)` pixels from the centre, with `f = (video_width / 2) / tan(hfov/2)`.
## A linear degrees-to-pixels scale would only line up near the centre -- which is
## where a horizon is least useful.

## Horizontal field of view, measured on the bench. The vertical one is not a
## second measurement: a rectilinear lens has one focal length in pixels, so the
## frame aspect ratio supplies it.
@export var camera_hfov_deg: float = 45.0

## Aspect ratio of the frames the camera sends. VGA and QVGA are both 4:3.
@export var camera_aspect: float = 4.0 / 3.0

@export var hud_color: Color = Color.GREEN

## /imu reports pitch positive nose-up and roll positive right-side-down. Flip
## these if the breakout ends up mounted the other way round.
@export var invert_pitch: bool = false
@export var invert_roll: bool = false

## A sample older than this is not drawn: a stalled sampler must not leave a
## plausible-looking horizon on screen.
@export var stale_after_s: float = 1.0

## One ladder rung every `ladder_step_deg`, out to `ladder_limit_deg` each way.
@export var ladder_step_deg: float = 10.0
@export var ladder_limit_deg: float = 30.0

## Bank scale: a tick every 10 degrees, `bank_limit_deg` either side of vertical.
@export var bank_limit_deg: float = 45.0

var pitch_deg: float = 0.0
var roll_deg: float = 0.0

var _has_sample := false
var _sample_ms := 0
var _view := Rect2()
var _focal_px := 1.0


## Latest attitude from the car, in degrees. Safe to call at whatever rate the
## samples arrive; the HUD only needs the newest one.
func set_attitude(pitch: float, roll: float) -> void:
	pitch_deg = pitch
	roll_deg = roll
	_has_sample = true
	_sample_ms = Time.get_ticks_msec()


## Forgets the last attitude: no sensor, no feed, or a board that said it has no
## IMU. The frame stays, so the overlay does not blink out with the data.
func clear_attitude() -> void:
	_has_sample = false


func _process(_delta: float) -> void:
	if is_visible_in_tree():
		queue_redraw()


func _draw() -> void:
	_view = _fit_inside(size, camera_aspect)
	_focal_px = (_view.size.x * 0.5) / tan(deg_to_rad(camera_hfov_deg) * 0.5)

	_draw_frame()
	_draw_reticle()

	if _is_fresh():
		_draw_horizon()
		_draw_ladder()
		_draw_bank_scale()
		_draw_readout()
	else:
		_draw_absent()


# The frame and everything drawn inside it covers the camera image, not the whole
# window: with the feed letterboxed the bars are not part of the world, so an
# attitude or scale drawn over them would be a lie about where the camera looks.
func _fit_inside(outer: Vector2, aspect: float) -> Rect2:
	var inner := Vector2(outer.y * aspect, outer.y)
	if inner.x > outer.x:
		inner = Vector2(outer.x, outer.x / aspect)
	return Rect2((outer - inner) * 0.5, inner)


func _is_fresh() -> bool:
	return _has_sample and Time.get_ticks_msec() - _sample_ms <= stale_after_s * 1000.0


## Pitch and roll after the mounting sign convention is applied.
func _attitude() -> Vector2:
	var pitch := -pitch_deg if invert_pitch else pitch_deg
	var roll := -roll_deg if invert_roll else roll_deg
	return Vector2(pitch, roll)


## Pixels below the optical axis for a direction `above_deg` above it.
func _below(above_deg: float) -> float:
	return -_focal_px * tan(deg_to_rad(above_deg))


func _draw_frame() -> void:
	var width := maxf(2.0, _view.size.y * 0.003)
	for x in [_view.position.x, _view.end.x]:
		draw_line(Vector2(x, _view.position.y), Vector2(x, _view.end.y), hud_color, width, true)


func _draw_reticle() -> void:
	var center := _view.get_center()
	var arm := _view.size.y * 0.022
	var width := maxf(1.5, _view.size.y * 0.0025)
	draw_circle(center, maxf(2.0, _view.size.y * 0.004), hud_color)
	for axis in [Vector2.RIGHT, Vector2.DOWN]:
		draw_line(center + axis * arm * 0.45, center + axis * arm, hud_color, width, true)
		draw_line(center - axis * arm * 0.45, center - axis * arm, hud_color, width, true)


## The horizon is found by pitch alone, then tilted by roll: two straight lines
## with a gap in the middle, and a cap at each end so the tilt stays readable when
## the line is almost edge-on.
func _draw_horizon() -> void:
	var attitude := _attitude()
	var pitch := attitude.x
	var roll := deg_to_rad(attitude.y)

	var anchor := _view.get_center() + Vector2(0.0, _below(-pitch)).rotated(-roll)
	if not _view.grow(_view.size.y * 0.05).has_point(anchor):
		return

	var along := Vector2(cos(roll), -sin(roll))
	var half := _view.size.x * 0.42
	var gap := _view.size.x * 0.06
	var cap := along.orthogonal() * _view.size.y * 0.018
	var width := maxf(2.0, _view.size.y * 0.0035)

	draw_line(anchor - along * half, anchor - along * gap, hud_color, width, true)
	draw_line(anchor + along * gap, anchor + along * half, hud_color, width, true)
	draw_line(anchor - along * half - cap, anchor - along * half + cap, hud_color, width, true)
	draw_line(anchor + along * half - cap, anchor + along * half + cap, hud_color, width, true)


## Rungs at fixed angles above and below the horizon, carried along with it, so a
## rung on the centre line reads the pitch directly.
func _draw_ladder() -> void:
	var attitude := _attitude()
	var pitch := attitude.x
	var roll := deg_to_rad(attitude.y)
	var along := Vector2(cos(roll), -sin(roll))

	var half := _view.size.x * 0.11
	var gap := _view.size.x * 0.035
	var width := maxf(1.0, _view.size.y * 0.0025)
	var color := Color(hud_color, hud_color.a * 0.75)
	var font := get_theme_default_font()
	var font_size := clampi(int(_view.size.y * 0.028), 10, 32)

	var rung := ladder_step_deg
	while rung <= ladder_limit_deg + 0.001:
		for direction in [1.0, -1.0]:
			var angle: float = direction * rung
			var anchor: Vector2 = (
				_view.get_center() + Vector2(0.0, _below(angle - pitch)).rotated(-roll)
			)
			if not _view.grow(_view.size.y * 0.05).has_point(anchor):
				continue

			draw_line(anchor - along * half, anchor - along * gap, color, width, true)
			draw_line(anchor + along * gap, anchor + along * half, color, width, true)

			var label := "%+d" % int(angle)
			var label_at := anchor + along * (half + gap * 0.6) + Vector2(0.0, font_size * 0.35)
			draw_string(
				font,
				label_at + Vector2(1.0, 1.0),
				label,
				HORIZONTAL_ALIGNMENT_LEFT,
				-1,
				font_size,
				Color(0.0, 0.0, 0.0, 0.5)
			)
			draw_string(font, label_at, label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, color)
		rung += ladder_step_deg


## A bank scale fixed to the camera, with a pointer that follows the car: roll is
## readable here even when pitch has pushed the horizon out of frame.
func _draw_bank_scale() -> void:
	var roll := deg_to_rad(_attitude().y)
	var center := _view.get_center()
	var radius := minf(_view.size.x, _view.size.y) * 0.34
	var up := -PI * 0.5
	var limit := deg_to_rad(bank_limit_deg)
	var tick := _view.size.y * 0.013
	var width := maxf(1.5, _view.size.y * 0.003)

	draw_arc(center, radius, up - limit, up + limit, 48, hud_color, width, true)

	var mark := -bank_limit_deg
	while mark <= bank_limit_deg + 0.001:
		var angle := up + deg_to_rad(mark)
		var length := tick * (2.0 if absi(int(mark)) % 30 == 0 else 1.0)
		var radial := Vector2(cos(angle), sin(angle))
		draw_line(
			center + radial * (radius - length),
			center + radial * (radius + length),
			hud_color,
			width,
			true
		)
		mark += 10.0

	var pointer := up + roll
	var inward := Vector2(cos(pointer), sin(pointer))
	var sideways := inward.orthogonal() * tick * 1.2
	draw_colored_polygon(
		PackedVector2Array(
			[
				center + inward * (radius - tick * 0.4),
				center + inward * (radius + tick * 2.4) + sideways,
				center + inward * (radius + tick * 2.4) - sideways,
			]
		),
		hud_color
	)


func _draw_readout() -> void:
	var attitude := _attitude()
	var font_size := clampi(int(_view.size.y * 0.032), 12, 36)
	var text := "PITCH %+.1f°    ROLL %+.1f°" % [attitude.x, attitude.y]
	# Tucked into the video's top-left, under the lights row: a 45° lens sees only
	# about 17° vertically, so the 10° rung already sits near the top edge and the
	# middle of the frame belongs to the horizon.
	var at := Vector2(
		_view.position.x + _view.size.x * 0.025, _view.position.y + _view.size.y * 0.115
	)
	_draw_label(text, at, font_size, hud_color)


func _draw_absent() -> void:
	var font := get_theme_default_font()
	var font_size := clampi(int(_view.size.y * 0.03), 12, 34)
	var text := "IMU unavailable"
	var text_size := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size)
	var at := Vector2(
		_view.get_center().x - text_size.x * 0.5, _view.get_center().y + font_size * 1.5
	)
	_draw_label(text, at, font_size, Color(hud_color, hud_color.a * 0.6))


## Text over a dark plate. The corner the readout lives in is crossed by the horizon
## at some pitches, and green on green is unreadable.
func _draw_label(text: String, at: Vector2, font_size: int, color: Color) -> void:
	var font := get_theme_default_font()
	var text_size := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size)
	var pad := Vector2(font_size * 0.4, font_size * 0.2)
	var plate := Rect2(
		at - Vector2(pad.x, font.get_ascent(font_size) + pad.y), text_size + pad * 2.0
	)
	draw_rect(plate, Color(0.0, 0.0, 0.0, 0.35), true)
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, color)
