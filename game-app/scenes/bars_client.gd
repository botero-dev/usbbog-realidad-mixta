class_name BarsClient
extends Node

## Sends brightness levels (1 .. MAX_BARS) to the ESP32 bar display.
##
## The firmware lives in ../network-test (Heltec WiFi LoRa 32 V3). The board
## runs its OWN access point, so whatever machine runs this game has to be
## joined to that network first:
##
##     SSID      network-test   (AP_SSID in network-test/include/secrets.h)
##     password  networktest    (AP_PASSWORD; "" means an open network)
##     endpoint  http://192.168.4.1/bars
##
## Usage:
##
##     var bars := BarsClient.new()
##     add_child(bars)
##     bars.levels_applied.connect(func(levels): print(levels))
##     bars.request_failed.connect(func(why): push_warning(why))
##     bars.set_levels(PackedFloat32Array([0.1, 0.3, 0.5, 0.7, 1.0]))
##
## The array length decides how many bars the panel paints (1 .. MAX_BARS); the
## bars always share the 128 px width equally, so more levels mean narrower bars.

## Emitted with the levels the board confirmed it applied.
signal levels_applied(levels: PackedFloat32Array)

## Emitted with a readable reason whenever a request does not succeed.
signal request_failed(reason: String)

## The most levels the firmware takes in one request. The count is free between
## 1 and this value - it is the array length that sets the bar count.
const MAX_BARS := 32

## An ESP32 always takes 192.168.4.1 on its own SoftAP.
const DEFAULT_ENDPOINT := "http://192.168.4.1/bars"

## Where to POST. Override it if you reach the board some other way.
var endpoint := DEFAULT_ENDPOINT

## Seconds to wait for an answer before giving up.
var timeout_seconds := 5.0

var _http: HTTPRequest
var _busy := false
var _queued := PackedFloat32Array()


func _ready() -> void:
	_http = HTTPRequest.new()
	_http.timeout = timeout_seconds
	_http.request_completed.connect(_on_request_completed)
	add_child(_http)


## Asks the board to paint the bars at `levels`, each in 0.0 .. 1.0
## (0.0 = every pixel off, 1.0 = solid white). Values are clamped.
##
## Requests never overlap: if one is still in flight the newest call is queued
## and sent as soon as the board answers, so dragging a slider stays responsive
## instead of flooding the ESP32 with connections.
func set_levels(levels: PackedFloat32Array) -> void:
	if levels.size() < 1 or levels.size() > MAX_BARS:
		request_failed.emit(
			"expected 1 to %d levels, got %d" % [MAX_BARS, levels.size()]
		)
		return

	var clamped := PackedFloat32Array()
	clamped.resize(levels.size())
	for i in levels.size():
		clamped[i] = clampf(levels[i], 0.0, 1.0)

	if _busy:
		_queued = clamped
		return
	_send(clamped)


func _send(levels: PackedFloat32Array) -> void:
	# Two decimals are plenty for a 64-step dither and keep the body short.
	var payload: Array = []
	for level in levels:
		payload.append(snappedf(level, 0.01))

	# The content type matters: without it the ESP32 parses the payload as form
	# data and never sees a JSON body.
	var headers := PackedStringArray(["Content-Type: application/json"])
	var error := _http.request(
		endpoint, headers, HTTPClient.METHOD_POST, JSON.stringify(payload)
	)
	if error != OK:
		request_failed.emit("could not start the request (error %d)" % error)
		return

	_busy = true


func _on_request_completed(
	result: int, response_code: int, _headers: PackedStringArray,
	body: PackedByteArray
) -> void:
	_busy = false

	if result != HTTPRequest.RESULT_SUCCESS:
		request_failed.emit("no answer from %s (result %d)" % [endpoint, result])
	else:
		_handle_reply(response_code, body.get_string_from_utf8())

	if not _queued.is_empty():
		var next := _queued
		_queued = PackedFloat32Array()
		_send(next)


func _handle_reply(response_code: int, text: String) -> void:
	var parsed: Variant = JSON.parse_string(text)

	if response_code != 200:
		var detail := text.strip_edges()
		if parsed is Dictionary and parsed.has("error"):
			detail = str(parsed["error"])
		request_failed.emit("HTTP %d: %s" % [response_code, detail])
		return

	if not (parsed is Dictionary and parsed.has("levels")):
		request_failed.emit("unexpected reply: %s" % text.strip_edges())
		return

	var levels := PackedFloat32Array()
	for value in parsed["levels"]:
		levels.append(float(value))
	levels_applied.emit(levels)
