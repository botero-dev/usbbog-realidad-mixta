extends Control

@onready var connect_view: Control = $ConnectView
@onready var control_view: Control = $ControlView
@onready var connect_button: Button = $ConnectView/Button
@onready var status_label: Label = $ConnectView/StatusLabel
@onready var video: TextureRect = $ControlView/TextureRect
@onready var camera: CameraSource = $CameraFeed

var _got_frame := false


func _ready() -> void:
	control_view.hide()
	connect_view.show()
	status_label.text = ""
	camera.frame_received.connect(_on_frame_received)
	camera.stream_opened.connect(_on_stream_opened)
	camera.stream_closed.connect(_on_stream_closed)


func _on_button_pressed() -> void:
	_got_frame = false
	connect_button.disabled = true
	connect_button.text = "Connecting to %s…" % camera.target()
	status_label.text = ""
	camera.start()


func _on_stream_opened() -> void:
	status_label.text = "Waiting for the first frame…"


# Switch to the control view on the first real frame, not on connect: the
# placeholder icon stays up while the stream is still being negotiated.
func _on_frame_received(texture: Texture2D) -> void:
	video.texture = texture
	if _got_frame:
		return
	_got_frame = true
	connect_view.hide()
	control_view.show()


func _on_stream_closed(reason: String) -> void:
	_got_frame = false
	control_view.hide()
	connect_view.show()
	connect_button.disabled = false
	connect_button.text = "Reconnect to %s" % camera.target()
	status_label.text = reason
