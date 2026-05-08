extends Control

@onready var player: VideoStreamPlayer = $VideoStreamPlayer
@onready var status_label: Label = $Overlay/StatusLabel
@onready var open_button: Button = $Overlay/ButtonRow/OpenButton
@onready var play_pause_button: Button = $Overlay/ButtonRow/PlayPauseButton
@onready var stop_button: Button = $Overlay/ButtonRow/StopButton
@onready var file_dialog: FileDialog = $FileDialog

func _ready() -> void:
	open_button.pressed.connect(_on_open_pressed)
	play_pause_button.pressed.connect(_on_play_pause_pressed)
	stop_button.pressed.connect(_on_stop_pressed)
	file_dialog.file_selected.connect(_on_file_selected)
	player.finished.connect(_on_finished)

func _process(_delta: float) -> void:
	if player.stream == null:
		return
	var pos: float = player.get_stream_position()
	var len: float = player.get_stream_length()
	status_label.text = "%.2f / %.2f sec" % [pos, len]

func _on_open_pressed() -> void:
	file_dialog.popup_centered()

func _on_file_selected(path: String) -> void:
	var stream: VideoStreamHap = VideoStreamHap.new()
	stream.file = path
	player.stream = stream
	player.play()
	play_pause_button.disabled = false
	stop_button.disabled = false
	play_pause_button.text = "Pause"
	status_label.text = "Playing: " + path.get_file()
	await get_tree().process_frame
	if player.stream != null and player.stream.is_ycocg():
		var mat := ShaderMaterial.new()
		mat.shader = load("res://addons/godot-hap/shaders/ycocg.gdshader")
		player.material = mat
	else:
		player.material = null

func _on_play_pause_pressed() -> void:
	if player.is_playing():
		player.paused = not player.paused
		play_pause_button.text = "Resume" if player.paused else "Pause"
	else:
		player.play()
		play_pause_button.text = "Pause"

func _on_stop_pressed() -> void:
	player.stop()
	play_pause_button.text = "Play"
	play_pause_button.disabled = true
	stop_button.disabled = true
	status_label.text = "Stopped"

func _on_finished() -> void:
	play_pause_button.text = "Play"
	status_label.text = "Finished"
