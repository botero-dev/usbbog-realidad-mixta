extends Control


# given as ratio from screen height
@export var hud_screen_perc: float = 0.8
# aspect ratio for hud
@export var hud_aspect: float  =  3.0 / 2.0

@export var hud_color: Color = Color.GREEN

var hud_base: float
var hud_size: Vector2
var screen_size: Vector2

# given as percent of hud_size
var pitch_ticks_length: float = 5
var pitch_ticks_width: float = 1

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	pass # Replace with function body.


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	screen_size = get_rect().size
	var hud_size_y := screen_size.y * hud_screen_perc
	hud_size = Vector2(hud_size_y * hud_aspect, hud_size_y)
	hud_base = hud_size_y * 0.01
	queue_redraw()

func _draw() -> void:
	var center := screen_size * 0.5
	var half_hud_size := (hud_size * 0.5)
	var start := center - half_hud_size
	var end := center + half_hud_size
	
	draw_line(start, Vector2(start.x, end.y), hud_color, pitch_ticks_width * hud_base, true)
	draw_line(Vector2(end.x, start.y), end, hud_color, pitch_ticks_width * hud_base, true)
	
	# todo: extend compass draw.
	
	# maybe do something more scientific considering camera fov?
	var ticks := 11 # use odd so one lands on center
	var gap := (end.y - start.y ) / (ticks - 1)
	var tick_length = pitch_ticks_length * hud_base
	for idx in range(ticks):
		var y := start.y + gap * idx
		draw_line(Vector2(start.x, y), Vector2(start.x + tick_length, y), hud_color, pitch_ticks_width * hud_base, true)
		draw_line(Vector2(end.x, y), Vector2(end.x - tick_length, y), hud_color, pitch_ticks_width * hud_base, true)
	
