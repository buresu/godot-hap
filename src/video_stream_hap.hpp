#pragma once

#include <godot_cpp/classes/video_stream.hpp>
#include <godot_cpp/classes/video_stream_playback.hpp>

namespace godot {

class VideoStreamPlaybackHap;

class VideoStreamHap : public VideoStream {
    GDCLASS(VideoStreamHap, VideoStream)

public:
    bool is_ycocg() const;
    Ref<VideoStreamPlayback> _instantiate_playback() override;

protected:
    static void _bind_methods();

private:
    Ref<VideoStreamPlaybackHap> _pb;
};

} // namespace godot
