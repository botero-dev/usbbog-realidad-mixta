class_name VirtualStick
extends Control

## A thumbstick whose origin is WHERE THE FINGER LANDED, not a fixed point.
##
## A fixed-centre stick has to be found before it can be used, and on a phone the
## thumb is usually already there -- so the first touch sets the origin and the
## deflection is measured from it. `axis` says which direction the stick may move in,
## which is what lets one control be a throttle (vertical) and another a wheel
## (horizontal) from the same code.
##
## `changed` reports the deflection along `axis` only: x is +right, y is +down, and
## the length is 0.0 .. 1.0 with `deadzone` rescaled away. A stick on the vertical axis
## therefore always reports x = 0.0, so a diagonal drag cannot smuggle in a command the
## control was not meant to make.
##
## Two pointers work at once: each stick claims its own touch index, so throttle and
## steering can be held together. Mouse events are also accepted, which is what makes
## this usable on a desktop (and what Godot synthesises from a single finger).

signal changed(value: Vector2)
signal released()

## Which components this stick may produce: Vector2(0, 1) for a vertical throttle,
## Vector2(1, 0) for a horizontal wheel, Vector2(1, 1) for a free stick.
@export var axis := Vector2(0, 1)
@export var deadzone: float = 0.12
@export var radius: float = 84.0
@export var knob_radius: float = 34.0
@export var label_text := ""
@export var base_color: Color = Color(0.05, 0.08, 0.12, 0.40)
@export var ring_color: Color = Color(0.3, 1.0, 0.3, 0.85)
@export var knob_color: Color = Color(0.3, 1.0, 0.3, 0.55)

var _value := Vector2.ZERO
var _origin := Vector2.ZERO
var _active := false
var _touch_index := -1
var _mouse_down := false


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP


func _input(event: InputEvent) -> void:
	if not is_visible_in_tree():
		return

	if event is InputEventScreenTouch:
		if event.pressed:
			# Claim the finger only if it landed in this stick and no other stick has
			# it: the two sticks must not fight over one touch.
			if _touch_index == -1 and get_global_rect().has_point(event.position):
				_touch_index = event.index
				_begin(event.position)
		elif event.index == _touch_index:
			_touch_index = -1
			_end()
		return

	if event is InputEventScreenDrag:
		if event.index == _touch_index:
			_follow(event.position)
		return

	# Mouse. On a touch device Godot also synthesises mouse events from the first
	# finger, so while a real touch owns this stick the emulated mouse is ignored --
	# otherwise releasing one of two fingers could end the other's drag.
	if _touch_index != -1:
		return

	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			if get_global_rect().has_point(event.position):
				_mouse_down = true
				_begin(event.position)
		elif _mouse_down:
			_mouse_down = false
			_end()
	elif event is InputEventMouseMotion and _mouse_down:
		_follow(event.position)


func _notification(what: int) -> void:
	# Hidden mid-drag (stream dropped, view switched): the release will never arrive.
	if what == NOTIFICATION_VISIBILITY_CHANGED and not is_visible_in_tree():
		reset()


func _begin(global_position_in_viewport: Vector2) -> void:
	_origin = _to_local(global_position_in_viewport)
	_active = true
	_value = Vector2.ZERO
	queue_redraw()
	changed.emit(_value)


func _follow(global_position_in_viewport: Vector2) -> void:
	if not _active:
		return

	var delta := _to_local(global_position_in_viewport) - _origin
	# Project onto the allowed axis first: a vertical stick sees only the part of the
	# drag that is vertical, so sliding sideways does not change the command.
	var along := Vector2(delta.x * axis.x, delta.y * axis.y)

	var length := along.length()
	if radius <= 0.0 or length <= 0.0:
		_apply(Vector2.ZERO)
		return

	var magnitude := minf(length / radius, 1.0)
	if magnitude < deadzone:
		_apply(Vector2.ZERO)
		return

	# Rescale so the deadzone edge is 0.0 and the rim is 1.0 -- otherwise the first
	# pixel past the deadzone would command a visible fraction of full speed.
	magnitude = (magnitude - deadzone) / (1.0 - deadzone)
	_apply(along / length * magnitude)


func _apply(v: Vector2) -> void:
	if v == _value:
		return
	_value = v
	queue_redraw()
	changed.emit(_value)


func _end() -> void:
	_active = false
	var was := _value
	_value = Vector2.ZERO
	queue_redraw()
	if was != Vector2.ZERO:
		changed.emit(_value)
	released.emit()


## Returns the stick to rest without a release event from the user.
func reset() -> void:
	_touch_index = -1
	_mouse_down = false
	if not _active and _value == Vector2.ZERO:
		return
	_end()


func value() -> Vector2:
	return _value


func is_held() -> bool:
	return _active


func _to_local(position_in_viewport: Vector2) -> Vector2:
	return get_global_transform().affine_inverse() * position_in_viewport


func _draw() -> void:
	if radius <= 0.0:
		return

	var held := _active
	var centre := _origin if held else size * 0.5
	var base := base_color
	var ring := ring_color

	draw_circle(centre, radius, base)
	draw_arc(centre, radius, 0.0, TAU, 48, ring, 2.0, true)

	# The axis this stick travels on, so its job is readable before it is touched.
	var guide := axis.normalized() * radius
	draw_line(centre - guide, centre + guide, ring * Color(1, 1, 1, 0.45), 1.0, true)

	draw_circle(centre + _value * radius, knob_radius, knob_color)
	draw_arc(centre + _value * radius, knob_radius, 0.0, TAU, 32, ring, 2.0, true)

	if not label_text.is_empty():
		var font := get_theme_default_font()
		if font != null:
			var font_size := 18
			var width := font.get_string_size(
				label_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size
			).x
			# Below the base when held (it has moved to the finger), above it at rest.
			var y := centre.y + radius + 22.0 if held else centre.y + radius - 12.0
			draw_string(
				font,
				Vector2(centre.x - width * 0.5, y),
				label_text,
				HORIZONTAL_ALIGNMENT_LEFT,
				-1,
				font_size,
				ring * Color(1, 1, 1, 0.75)
			)
