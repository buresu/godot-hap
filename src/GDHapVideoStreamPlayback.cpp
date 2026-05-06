#define MINIMP4_IMPLEMENTATION
#include "GDHapVideoStreamPlayback.hpp"

#include <algorithm>

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
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

RenderingDevice::DataFormat GDHapVideoStreamPlayback::hap_to_rd_format(unsigned int hap_format) {
    switch (hap_format) {
        case HapTextureFormat_RGB_DXT1:
            return RenderingDevice::DATA_FORMAT_BC1_RGB_UNORM_BLOCK;
        case HapTextureFormat_RGBA_DXT5:
        case HapTextureFormat_YCoCg_DXT5:
            return RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK;
        case HapTextureFormat_A_RGTC1:
            return RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK;
        case HapTextureFormat_RGBA_BPTC_UNORM:
            return RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK;
        case HapTextureFormat_RGB_BPTC_UNSIGNED_FLOAT:
            return RenderingDevice::DATA_FORMAT_BC6H_UFLOAT_BLOCK;
        case HapTextureFormat_RGB_BPTC_SIGNED_FLOAT:
            return RenderingDevice::DATA_FORMAT_BC6H_SFLOAT_BLOCK;
        default:
            return RenderingDevice::DATA_FORMAT_MAX;
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
// Scan file for HAP VisualSampleEntry to extract width/height.
// minimp4 skips unknown codec boxes, so HAP track dimensions stay 0.
// The VisualSampleEntry structure has width at byte 32 and height at byte 34
// from the start of the box (FourCC is at byte 4), i.e., FourCC+28 / FourCC+30.
// ---------------------------------------------------------------------------

bool GDHapVideoStreamPlayback::read_hap_dimensions() {
    static const uint8_t hap_prefixes[][3] = {
        {0x48, 0x61, 0x70},  // 'Hap' prefix for Hap1, Hap5, HapY, HapM, HapA
    };
    static const uint8_t hap_suffixes[] = {0x31, 0x35, 0x59, 0x4D, 0x41};  // 1, 5, Y, M, A

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);

    const long scan_block = 2 * 1024 * 1024;
    // Try end of file first (moov is usually at end for recorded files)
    // Then try beginning (faststart/web-optimized files)
    long scan_offsets[] = {
        std::max(0L, fsize - scan_block),
        0L
    };

    for (long scan_start : scan_offsets) {
        long avail = fsize - scan_start;
        if (avail <= 0) continue;
        size_t buf_size = static_cast<size_t>(std::min(avail, scan_block));
        std::vector<uint8_t> buf(buf_size);
        fseek(file, scan_start, SEEK_SET);
        if (fread(buf.data(), 1, buf_size, file) != buf_size) continue;

        for (size_t i = 4; i + 32 < buf_size; i++) {
            if (buf[i] != 0x48 || buf[i+1] != 0x61 || buf[i+2] != 0x70) continue;
            bool is_hap = false;
            for (uint8_t suf : hap_suffixes) {
                if (buf[i+3] == suf) { is_hap = true; break; }
            }
            if (!is_hap) continue;

            int w = (buf[i+28] << 8) | buf[i+29];
            int h = (buf[i+30] << 8) | buf[i+31];
            if (w > 0 && h > 0 && w <= 65535 && h <= 65535) {
                width = w;
                height = h;
                return true;
            }
        }
    }
    return false;
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
        UtilityFunctions::push_error("GDHapVideoStream: HapGetFrameTextureFormat failed at frame ", index);
        return;
    }

    RenderingDevice::DataFormat fmt = hap_to_rd_format(hap_format);
    if (fmt == RenderingDevice::DATA_FORMAT_MAX) {
        UtilityFunctions::push_error("GDHapVideoStream: unsupported HAP format ", (int)hap_format);
        return;
    }

    // Fallback: derive dimensions from compressed block count if stsd had no width/height
    if (width == 0 || height == 0) {
        UtilityFunctions::push_error("GDHapVideoStream: width/height is 0, cannot decode frame");
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

    RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
    if (!rd || !texture_rid.is_valid()) {
        return;
    }

    PackedByteArray pba;
    pba.resize(static_cast<int64_t>(bytes_used));
    memcpy(pba.ptrw(), decode_buf.data(), bytes_used);
    rd->texture_update(texture_rid, 0, pba);
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

    // Read first box header for diagnostics
    char hdr[8] = {};
    fread(hdr, 1, 8, file);
    fseek(file, 0, SEEK_SET);
    String first_box = String::utf8(hdr + 4, 4);

    mp4 = {};
    if (!MP4D_open(&mp4, read_callback, file, file_size)) {
        UtilityFunctions::push_error(
                "GDHapVideoStream: failed to parse MP4 [first_box=", first_box,
                " file_size=", (int64_t)file_size, "]: ", p_path);
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
    // Some MOV files have a second hdlr (data handler 'url ') inside minf that
    // overwrites the media handler type set by the hdlr in mdia. Fall back to
    // finding any track that has samples and no audio-specific fields set.
    if (video_track < 0) {
        for (unsigned int i = 0; i < mp4.track_count; i++) {
            const MP4D_track_t &tr = mp4.track[i];
            if (tr.sample_count > 0 &&
                    tr.SampleDescription.audio.channelcount == 0 &&
                    tr.SampleDescription.audio.samplerate_hz == 0) {
                video_track = static_cast<int>(i);
                break;
            }
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
    if ((width == 0 || height == 0) && !read_hap_dimensions()) {
        UtilityFunctions::push_error("GDHapVideoStream: cannot determine video dimensions: ", p_path);
        MP4D_close(&mp4);
        fclose(file);
        file = nullptr;
        return;
    }
    frame_count = track.sample_count;

    UtilityFunctions::print("GDHapVideoStream: video_track=", video_track,
            " size=", width, "x", height, " frames=", frame_count);

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

    // Pre-allocate a GPU texture via RenderingDevice.
    // Detect HAP format from the first frame.
    // All formats (including YCoCg_DXT5) are uploaded as compressed BC textures;
    // Hap Q color conversion is handled by a GPU shader.
    {
        unsigned int hap_fmt = HapTextureFormat_RGB_DXT1;
        if (!frames.empty()) {
            std::vector<uint8_t> probe(frames[0].size);
            fseek(file, static_cast<long>(frames[0].offset), SEEK_SET);
            if (fread(probe.data(), 1, frames[0].size, file) == frames[0].size) {
                unsigned int detected = 0;
                if (HapGetFrameTextureFormat(probe.data(), frames[0].size, 0, &detected) == HapResult_No_Error) {
                    hap_fmt = detected;
                }
            }
        }
        video_hap_format = hap_fmt;

        RenderingDevice::DataFormat rd_fmt = hap_to_rd_format(hap_fmt);
        size_t blank_size = dxt_buffer_size(hap_fmt, width, height);

        RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
        if (rd) {
            PackedByteArray blank;
            blank.resize(static_cast<int64_t>(blank_size));
            blank.fill(0);

            Ref<RDTextureFormat> tf;
            tf.instantiate();
            tf->set_format(rd_fmt);
            tf->set_width(static_cast<uint32_t>(width));
            tf->set_height(static_cast<uint32_t>(height));
            tf->set_depth(1);
            tf->set_array_layers(1);
            tf->set_mipmaps(1);
            tf->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
            tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
                               RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);

            TypedArray<PackedByteArray> init_data;
            init_data.push_back(blank);

            Ref<RDTextureView> tv;
            tv.instantiate();
            texture_rid = rd->texture_create(tf, tv, init_data);

            texture.instantiate();
            texture->set_texture_rd_rid(texture_rid);
        }
    }
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

GDHapVideoStreamPlayback::~GDHapVideoStreamPlayback() {
    if (texture_rid.is_valid()) {
        RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
        if (rd) {
            rd->free_rid(texture_rid);
        }
        texture_rid = RID();
    }
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

bool GDHapVideoStreamPlayback::is_ycocg() const {
    return video_hap_format == HapTextureFormat_YCoCg_DXT5;
}

void GDHapVideoStreamPlayback::_bind_methods() {
    ClassDB::bind_method(D_METHOD("is_ycocg"), &GDHapVideoStreamPlayback::is_ycocg);
}
