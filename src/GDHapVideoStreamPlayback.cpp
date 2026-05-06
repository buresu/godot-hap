#define MINIMP4_IMPLEMENTATION
#include "GDHapVideoStreamPlayback.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// ---------------------------------------------------------------------------
// minimp4 file read callback
// ---------------------------------------------------------------------------

int GDHapVideoStreamPlayback::read_callback(int64_t offset, void *buffer, size_t size, void *token) {
    FILE *f = static_cast<FILE *>(token);
    if (fseek(f, static_cast<long>(offset), SEEK_SET)) {
        return 1;
    }
    return fread(buffer, 1, size, f) != size;
}

// ---------------------------------------------------------------------------
// HAP single-threaded decode callback (from hap.h documentation)
// ---------------------------------------------------------------------------

void GDHapVideoStreamPlayback::hap_decode_callback(
        HapDecodeWorkFunction function, void *p, unsigned int count, void *info) {
    for (unsigned int i = 0; i < count; i++) {
        function(p, i);
    }
}

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

Image::Format GDHapVideoStreamPlayback::hap_to_godot_format(unsigned int hap_format) {
    switch (hap_format) {
        case HapTextureFormat_RGB_DXT1:
            return Image::FORMAT_DXT1;
        case HapTextureFormat_RGBA_DXT5:
        case HapTextureFormat_YCoCg_DXT5:
            return Image::FORMAT_DXT5;
        case HapTextureFormat_A_RGTC1:
            return Image::FORMAT_RGTC_R;
        case HapTextureFormat_RGBA_BPTC_UNORM:
            return Image::FORMAT_BPTC_RGBA;
        case HapTextureFormat_RGB_BPTC_UNSIGNED_FLOAT:
            return Image::FORMAT_BPTC_RGBFU;
        case HapTextureFormat_RGB_BPTC_SIGNED_FLOAT:
            return Image::FORMAT_BPTC_RGBF;
        default:
            return Image::FORMAT_MAX;
    }
}

size_t GDHapVideoStreamPlayback::dxt_buffer_size(unsigned int hap_format, int w, int h) {
    int block_bytes = 8;
    if (hap_format == HapTextureFormat_RGBA_DXT5 ||
            hap_format == HapTextureFormat_YCoCg_DXT5 ||
            hap_format == HapTextureFormat_RGBA_BPTC_UNORM ||
            hap_format == HapTextureFormat_RGB_BPTC_UNSIGNED_FLOAT ||
            hap_format == HapTextureFormat_RGB_BPTC_SIGNED_FLOAT) {
        block_bytes = 16;
    }
    return static_cast<size_t>(((w + 3) / 4) * ((h + 3) / 4)) * block_bytes;
}

// ---------------------------------------------------------------------------
// Seek helper: find frame index covering p_time
// ---------------------------------------------------------------------------

int GDHapVideoStreamPlayback::find_frame(double p_time) const {
    if (frames.empty()) {
        return 0;
    }
    int lo = 0, hi = static_cast<int>(frames.size()) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (frames[mid].time <= p_time) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

// ---------------------------------------------------------------------------
// Decode one frame and update texture
// ---------------------------------------------------------------------------

void GDHapVideoStreamPlayback::decode_frame(int index) {
    if (index < 0 || index >= static_cast<int>(frames.size())) {
        return;
    }
    const FrameInfo &fi = frames[index];

    read_buf.resize(fi.size);
    fseek(file, static_cast<long>(fi.offset), SEEK_SET);
    if (fread(read_buf.data(), 1, fi.size, file) != fi.size) {
        return;
    }

    unsigned int hap_format = 0;
    if (HapGetFrameTextureFormat(read_buf.data(), fi.size, 0, &hap_format) != HapResult_No_Error) {
        return;
    }

    Image::Format fmt = hap_to_godot_format(hap_format);
    if (fmt == Image::FORMAT_MAX) {
        return;
    }

    size_t out_size = dxt_buffer_size(hap_format, width, height);
    decode_buf.resize(out_size);

    unsigned long bytes_used = 0;
    unsigned int out_format = 0;
    unsigned int result = HapDecode(
            read_buf.data(), fi.size,
            0,
            hap_decode_callback, nullptr,
            decode_buf.data(), static_cast<unsigned long>(out_size),
            &bytes_used,
            &out_format);

    if (result != HapResult_No_Error) {
        return;
    }

    PackedByteArray pba;
    pba.resize(static_cast<int64_t>(bytes_used));
    memcpy(pba.ptrw(), decode_buf.data(), bytes_used);

    Ref<Image> image = Image::create_from_data(width, height, false, fmt, pba);
    if (image.is_null()) {
        return;
    }

    if (texture.is_null()) {
        texture = ImageTexture::create_from_image(image);
        godot_format = fmt;
    } else {
        texture->update(image);
    }
}

// ---------------------------------------------------------------------------
// open(): parse MP4 and build frame table
// ---------------------------------------------------------------------------

void GDHapVideoStreamPlayback::open(const String &p_path) {
    if (file) {
        MP4D_close(&mp4);
        fclose(file);
        file = nullptr;
    }

    file = fopen(p_path.utf8().get_data(), "rb");
    if (!file) {
        UtilityFunctions::push_error("GDHapVideoStream: cannot open file: ", p_path);
        return;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    mp4 = {};
    if (!MP4D_open(&mp4, read_callback, file, file_size)) {
        UtilityFunctions::push_error("GDHapVideoStream: failed to parse MP4: ", p_path);
        fclose(file);
        file = nullptr;
        return;
    }

    video_track = -1;
    for (unsigned int i = 0; i < mp4.track_count; i++) {
        if (mp4.track[i].handler_type == MP4D_HANDLER_TYPE_VIDE) {
            video_track = static_cast<int>(i);
            break;
        }
    }
    if (video_track < 0) {
        UtilityFunctions::push_error("GDHapVideoStream: no video track found in: ", p_path);
        MP4D_close(&mp4);
        fclose(file);
        file = nullptr;
        return;
    }

    const MP4D_track_t &track = mp4.track[video_track];
    width = static_cast<int>(track.SampleDescription.video.width);
    height = static_cast<int>(track.SampleDescription.video.height);
    frame_count = track.sample_count;

    double timescale = static_cast<double>(track.timescale);

    frames.resize(frame_count);
    double accumulated = 0.0;
    for (unsigned int i = 0; i < frame_count; i++) {
        unsigned frame_bytes = 0, timestamp = 0, duration = 0;
        MP4D_file_offset_t offset = MP4D_frame_offset(&mp4, video_track, i, &frame_bytes, &timestamp, &duration);
        frames[i].offset = offset;
        frames[i].size = frame_bytes;
        frames[i].time = static_cast<double>(timestamp) / timescale;
        frames[i].duration = static_cast<double>(duration) / timescale;
        accumulated = frames[i].time + frames[i].duration;
    }
    total_duration = accumulated;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

GDHapVideoStreamPlayback::~GDHapVideoStreamPlayback() {
    if (file) {
        MP4D_close(&mp4);
        fclose(file);
        file = nullptr;
    }
}

// ---------------------------------------------------------------------------
// VideoStreamPlayback overrides
// ---------------------------------------------------------------------------

void GDHapVideoStreamPlayback::_play() {
    if (!file) {
        return;
    }
    time = 0.0;
    current_frame = -1;
    playing = true;
    paused = false;
}

void GDHapVideoStreamPlayback::_stop() {
    playing = false;
    paused = false;
    time = 0.0;
    current_frame = -1;
}

bool GDHapVideoStreamPlayback::_is_playing() const {
    return playing && !paused;
}

void GDHapVideoStreamPlayback::_set_paused(bool p_paused) {
    paused = p_paused;
}

bool GDHapVideoStreamPlayback::_is_paused() const {
    return paused;
}

double GDHapVideoStreamPlayback::_get_length() const {
    return total_duration;
}

double GDHapVideoStreamPlayback::_get_playback_position() const {
    return time;
}

void GDHapVideoStreamPlayback::_seek(double p_time) {
    time = CLAMP(p_time, 0.0, total_duration);
    current_frame = -1;
}

void GDHapVideoStreamPlayback::_update(double p_delta) {
    if (!playing || paused || !file) {
        return;
    }

    time += p_delta;

    if (time >= total_duration) {
        time = 0.0;
        current_frame = -1;
    }

    int target = find_frame(time);
    if (target == current_frame) {
        return;
    }

    current_frame = target;
    decode_frame(current_frame);
}

Ref<Texture2D> GDHapVideoStreamPlayback::_get_texture() const {
    return texture;
}

// ---------------------------------------------------------------------------
// GDExtension binding
// ---------------------------------------------------------------------------

void GDHapVideoStreamPlayback::_bind_methods() {}
