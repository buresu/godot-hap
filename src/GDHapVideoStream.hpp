#pragma once

#include <godot_cpp/classes/video_stream.hpp>
#include <godot_cpp/classes/video_stream_playback.hpp>

using namespace godot;

class GDHapVideoStream : public VideoStream {
    GDCLASS(GDHapVideoStream, VideoStream)

protected:
    static void _bind_methods();

public:
    Ref<VideoStreamPlayback> _instantiate_playback() override;
};
