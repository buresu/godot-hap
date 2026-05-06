#pragma once

#include <godot_cpp/classes/video_stream.hpp>
#include <godot_cpp/classes/video_stream_playback.hpp>

namespace godot {

class GDHapVideoStreamPlayback;

class GDHapVideoStream : public VideoStream {
    GDCLASS(GDHapVideoStream, VideoStream)

public:
    bool is_ycocg() const;
    Ref<VideoStreamPlayback> _instantiate_playback() override;

protected:
    static void _bind_methods();

private:
    Ref<GDHapVideoStreamPlayback> _pb;
};

} // namespace godot
