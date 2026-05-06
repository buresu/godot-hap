#include "GDHapVideoStream.hpp"
#include "GDHapVideoStreamPlayback.hpp"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void GDHapVideoStream::_bind_methods() {}

Ref<VideoStreamPlayback> GDHapVideoStream::_instantiate_playback() {
    Ref<GDHapVideoStreamPlayback> pb;
    pb.instantiate();
    pb->open(get_file());
    return pb;
}
