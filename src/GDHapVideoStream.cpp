#include "GDHapVideoStream.hpp"
#include "GDHapVideoStreamPlayback.hpp"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void GDHapVideoStream::_bind_methods() {
    ClassDB::bind_method(D_METHOD("is_ycocg"), &GDHapVideoStream::is_ycocg);
}

bool GDHapVideoStream::is_ycocg() const {
    return _pb.is_valid() && _pb->is_ycocg();
}

Ref<VideoStreamPlayback> GDHapVideoStream::_instantiate_playback() {
    _pb = Ref<GDHapVideoStreamPlayback>();
    _pb.instantiate();
    _pb->open(get_file());
    return _pb;
}
