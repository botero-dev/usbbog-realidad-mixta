class_name CameraSource
extends Node

## Base class for a video feed coming from the car.
##
## The UI only ever talks to this interface, so the transport can change (MJPEG
## over TCP today, UDP later) without touching the scene or main_view.gd.
##
## Usage:
##
##     var camera: CameraSource = $CameraFeed
##     camera.frame_received.connect(func(texture): video.texture = texture)
##     camera.stream_closed.connect(func(reason): push_warning(reason))
##     camera.start()

## Emitted once the transport is connected and the server accepted the request.
signal stream_opened()

## Emitted whenever the feed stops, with a human-readable reason.
signal stream_closed(reason: String)

## Emitted for every decoded frame. The texture is reused between frames, so
## assign it to a TextureRect rather than keeping references to old ones.
signal frame_received(texture: Texture2D)

## Measured input frame rate, updated about once per second.
var fps: float = 0.0


## Starts (or restarts) the feed. Implementations must be safe to call again.
func start() -> void:
	push_error("CameraSource.start() must be overridden")


## Stops the feed without emitting stream_closed(): the caller asked for it.
func stop() -> void:
	pass


## True while frames are expected to arrive.
func is_streaming() -> bool:
	return false


## Short target shown in the UI, e.g. "192.168.4.1".
func target() -> String:
	return ""
