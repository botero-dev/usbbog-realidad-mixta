extends Control

func _on_button_pressed() -> void:
	var http := HTTPRequest.new()
	add_child(http)
	http.request_completed.connect(on_request_completed)
	http.request(
		"http://192.168.4.1/bars", 
		["Content-Type: application/json"],
		HTTPClient.METHOD_POST, 
		"[0.1, 0.3, 0.5, 0.7]"
	)

func on_request_completed(result: int, response_code: int, headers: PackedStringArray, body: PackedByteArray):
	print(result, response_code, headers, body.get_string_from_ascii())

func _on_button_2_pressed() -> void:
	var http := HTTPRequest.new()
	add_child(http)
	http.request(
		"http://192.168.4.1/bars", 
		["Content-Type: application/json"],
		HTTPClient.METHOD_POST, 
		"[1, 0.7, 0.5, 0.3, 0, 0.3, 0.5, 0.7, 1]"
	)


func _on_button_3_pressed() -> void:
	var http := HTTPRequest.new()
	add_child(http)
	http.request(
		"http://192.168.4.1/bars", 
		["Content-Type: application/json"],
		HTTPClient.METHOD_POST, 
		"[1, 0, 0.5, 0, 1.0]"
	)
