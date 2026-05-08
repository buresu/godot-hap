#include "VideoStreamHap.hpp"
#include "VideoStreamPlaybackHap.hpp"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void VideoStreamHap::_bind_methods() {
    ClassDB::bind_method(D_METHOD("is_ycocg"), &VideoStreamHap::is_ycocg);
}

bool VideoStreamHap::is_ycocg() const {
    return _pb.is_valid() && _pb->is_ycocg();
}

Ref<VideoStreamPlayback> VideoStreamHap::_instantiate_playback() {
    _pb = Ref<VideoStreamPlaybackHap>();
    _pb.instantiate();
    _pb->open(get_file());
    return _pb;
}
