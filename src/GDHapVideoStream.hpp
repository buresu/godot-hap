#pragma once

#include <godot_cpp/classes/video_stream.hpp>
#include <godot_cpp/classes/video_stream_playback.hpp>

class GDHapVideoStreamPlayback;

using namespace godot;

class GDHapVideoStream : public VideoStream {
    GDCLASS(GDHapVideoStream, VideoStream)

protected:
    static void _bind_methods();

public:
    bool is_ycocg() const;
    Ref<VideoStreamPlayback> _instantiate_playback() override;

private:
    Ref<GDHapVideoStreamPlayback> _pb;
};
