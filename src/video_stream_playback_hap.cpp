#define MINIMP4_IMPLEMENTATION
#include "video_stream_playback_hap.hpp"

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// ---------------------------------------------------------------------------
// minimp4 file read callback
// ---------------------------------------------------------------------------

int VideoStreamPlaybackHap::read_callback(int64_t offset, void *buffer, size_t size, void *token) {
    FileAccess *fa = static_cast<FileAccess *>(token);
    fa->seek(static_cast<uint64_t>(offset));
    return fa->get_buffer(static_cast<uint8_t *>(buffer), static_cast<uint64_t>(size)) != size;
}

// ---------------------------------------------------------------------------
// HAP single-threaded decode callback (from hap.h documentation)
// ---------------------------------------------------------------------------

void VideoStreamPlaybackHap::hap_decode_callback(
        HapDecodeWorkFunction function, void *p, unsigned int count, void *info) {
    for (unsigned int i = 0; i < count; i++) {
        function(p, i);
    }
}

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

RenderingDevice::DataFormat VideoStreamPlaybackHap::hap_to_rd_format(unsigned int hap_format) {
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

size_t VideoStreamPlaybackHap::dxt_buffer_size(unsigned int hap_format, int w, int h) {
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

bool VideoStreamPlaybackHap::read_hap_dimensions() {
    static const uint8_t hap_suffixes[] = {0x31, 0x35, 0x59, 0x4D, 0x41};  // 1, 5, Y, M, A

    int64_t fsize = static_cast<int64_t>(_file->get_length());

    const int64_t scan_block = 2 * 1024 * 1024;
    // Try end of file first (moov is usually at end for recorded files)
    // Then try beginning (faststart/web-optimized files)
    int64_t scan_offsets[] = {
        MAX(int64_t(0), fsize - scan_block),
        int64_t(0)
    };

    for (int64_t scan_start : scan_offsets) {
        int64_t avail = fsize - scan_start;
        if (avail <= 0) continue;
        int64_t buf_size = MIN(avail, scan_block);

        PackedByteArray buf;
        buf.resize(buf_size);
        _file->seek(static_cast<uint64_t>(scan_start));
        if (_file->get_buffer(buf.ptrw(), buf_size) != static_cast<uint64_t>(buf_size)) continue;

        const uint8_t *p = buf.ptr();
        for (int64_t i = 4; i + 32 < buf_size; i++) {
            if (p[i] != 0x48 || p[i+1] != 0x61 || p[i+2] != 0x70) continue;
            bool is_hap = false;
            for (uint8_t suf : hap_suffixes) {
                if (p[i+3] == suf) { is_hap = true; break; }
            }
            if (!is_hap) continue;

            int w = (p[i+28] << 8) | p[i+29];
            int h = (p[i+30] << 8) | p[i+31];
            if (w > 0 && h > 0 && w <= 65535 && h <= 65535) {
                _width = w;
                _height = h;
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Seek helper: find frame index covering p_time
// ---------------------------------------------------------------------------

int VideoStreamPlaybackHap::find_frame(double p_time) const {
    if (_frames.is_empty()) {
        return 0;
    }
    int lo = 0, hi = static_cast<int>(_frames.size()) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (_frames[mid].time <= p_time) {
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

void VideoStreamPlaybackHap::decode_frame(int index) {
    if (index < 0 || index >= static_cast<int>(_frames.size())) {
        return;
    }
    const FrameInfo &fi = _frames[index];

    _read_buf.resize(fi.size);
    _file->seek(fi.offset);
    if (_file->get_buffer(_read_buf.ptrw(), fi.size) != fi.size) {
        return;
    }

    unsigned int hap_format = 0;
    if (HapGetFrameTextureFormat(_read_buf.ptr(), fi.size, 0, &hap_format) != HapResult_No_Error) {
        UtilityFunctions::push_error("VideoStreamHap: HapGetFrameTextureFormat failed at frame ", index);
        return;
    }

    RenderingDevice::DataFormat fmt = hap_to_rd_format(hap_format);
    if (fmt == RenderingDevice::DATA_FORMAT_MAX) {
        UtilityFunctions::push_error("VideoStreamHap: unsupported HAP format ", (int)hap_format);
        return;
    }

    if (_width == 0 || _height == 0) {
        UtilityFunctions::push_error("VideoStreamHap: width/height is 0, cannot decode frame");
        return;
    }

    size_t out_size = dxt_buffer_size(hap_format, _width, _height);
    _decode_buf.resize(static_cast<int64_t>(out_size));

    unsigned long bytes_used = 0;
    unsigned int out_format = 0;
    unsigned int result = HapDecode(
            _read_buf.ptr(), fi.size,
            0,
            hap_decode_callback, nullptr,
            _decode_buf.ptrw(), static_cast<unsigned long>(out_size),
            &bytes_used,
            &out_format);

    if (result != HapResult_No_Error) {
        return;
    }

    RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
    if (!rd || !_texture_rid.is_valid()) {
        return;
    }

    _decode_buf.resize(static_cast<int64_t>(bytes_used));
    rd->texture_update(_texture_rid, 0, _decode_buf);
}

// ---------------------------------------------------------------------------
// open(): parse MP4 and build frame table
// ---------------------------------------------------------------------------

void VideoStreamPlaybackHap::open(const String &p_path) {
    if (_file.is_valid()) {
        MP4D_close(&_mp4);
        _file.unref();
    }

    _file = FileAccess::open(p_path, FileAccess::READ);
    if (!_file.is_valid()) {
        UtilityFunctions::push_error("VideoStreamHap: cannot open file: ", p_path);
        return;
    }

    int64_t file_size = static_cast<int64_t>(_file->get_length());

    // Read first box header for diagnostics
    uint8_t hdr[8] = {};
    _file->get_buffer(hdr, 8);
    _file->seek(0);
    String first_box = String::utf8(reinterpret_cast<const char *>(hdr + 4), 4);

    _mp4 = {};
    if (!MP4D_open(&_mp4, read_callback, _file.ptr(), file_size)) {
        UtilityFunctions::push_error(
                "VideoStreamHap: failed to parse MP4 [first_box=", first_box,
                " file_size=", file_size, "]: ", p_path);
        _file.unref();
        return;
    }

    _video_track = -1;
    for (unsigned int i = 0; i < _mp4.track_count; i++) {
        if (_mp4.track[i].handler_type == MP4D_HANDLER_TYPE_VIDE) {
            _video_track = static_cast<int>(i);
            break;
        }
    }
    // Some MOV files have a second hdlr (data handler 'url ') inside minf that
    // overwrites the media handler type set by the hdlr in mdia. Fall back to
    // finding any track that has samples and no audio-specific fields set.
    if (_video_track < 0) {
        for (unsigned int i = 0; i < _mp4.track_count; i++) {
            const MP4D_track_t &tr = _mp4.track[i];
            if (tr.sample_count > 0 &&
                    tr.SampleDescription.audio.channelcount == 0 &&
                    tr.SampleDescription.audio.samplerate_hz == 0) {
                _video_track = static_cast<int>(i);
                break;
            }
        }
    }
    if (_video_track < 0) {
        UtilityFunctions::push_error("VideoStreamHap: no video track found in: ", p_path);
        MP4D_close(&_mp4);
        _file.unref();
        return;
    }

    const MP4D_track_t &track = _mp4.track[_video_track];
    _width = static_cast<int>(track.SampleDescription.video.width);
    _height = static_cast<int>(track.SampleDescription.video.height);
    if ((_width == 0 || _height == 0) && !read_hap_dimensions()) {
        UtilityFunctions::push_error("VideoStreamHap: cannot determine video dimensions: ", p_path);
        MP4D_close(&_mp4);
        _file.unref();
        return;
    }
    _frame_count = track.sample_count;

    double timescale = static_cast<double>(track.timescale);

    _frames.resize(_frame_count);
    double accumulated = 0.0;
    for (unsigned int i = 0; i < _frame_count; i++) {
        unsigned frame_bytes = 0, timestamp = 0, duration = 0;
        MP4D_file_offset_t offset = MP4D_frame_offset(&_mp4, _video_track, i, &frame_bytes, &timestamp, &duration);
        _frames[i].offset = offset;
        _frames[i].size = frame_bytes;
        _frames[i].time = static_cast<double>(timestamp) / timescale;
        _frames[i].duration = static_cast<double>(duration) / timescale;
        accumulated = _frames[i].time + _frames[i].duration;
    }
    _total_duration = accumulated;

    // Pre-allocate a GPU texture via RenderingDevice.
    // Detect HAP format from the first frame.
    // All formats (including YCoCg_DXT5) are uploaded as compressed BC textures;
    // Hap Q color conversion is handled by a GPU shader.
    {
        unsigned int hap_fmt = HapTextureFormat_RGB_DXT1;
        if (!_frames.is_empty()) {
            _read_buf.resize(_frames[0].size);
            _file->seek(_frames[0].offset);
            if (_file->get_buffer(_read_buf.ptrw(), _frames[0].size) == _frames[0].size) {
                unsigned int detected = 0;
                if (HapGetFrameTextureFormat(_read_buf.ptr(), _frames[0].size, 0, &detected) == HapResult_No_Error) {
                    hap_fmt = detected;
                }
            }
        }
        _video_hap_format = hap_fmt;

        RenderingDevice::DataFormat rd_fmt = hap_to_rd_format(hap_fmt);
        size_t blank_size = dxt_buffer_size(hap_fmt, _width, _height);

        RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
        if (rd) {
            PackedByteArray blank;
            blank.resize(static_cast<int64_t>(blank_size));
            blank.fill(0);

            Ref<RDTextureFormat> tf;
            tf.instantiate();
            tf->set_format(rd_fmt);
            tf->set_width(static_cast<uint32_t>(_width));
            tf->set_height(static_cast<uint32_t>(_height));
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
            _texture_rid = rd->texture_create(tf, tv, init_data);

            _texture.instantiate();
            _texture->set_texture_rd_rid(_texture_rid);
        }
    }

    decode_frame(0);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

VideoStreamPlaybackHap::~VideoStreamPlaybackHap() {
    if (_texture_rid.is_valid()) {
        RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
        if (rd) {
            rd->free_rid(_texture_rid);
        }
        _texture_rid = RID();
    }
    if (_file.is_valid()) {
        MP4D_close(&_mp4);
    }
}

// ---------------------------------------------------------------------------
// VideoStreamPlayback overrides
// ---------------------------------------------------------------------------

void VideoStreamPlaybackHap::_play() {
    if (!_file.is_valid()) {
        return;
    }
    _time = 0.0;
    _current_frame = -1;
    _playing = true;
    _paused = false;
}

void VideoStreamPlaybackHap::_stop() {
    _playing = false;
    _paused = false;
    _time = 0.0;
    _current_frame = -1;
}

bool VideoStreamPlaybackHap::_is_playing() const {
    return _playing && !_paused;
}

void VideoStreamPlaybackHap::_set_paused(bool p_paused) {
    _paused = p_paused;
}

bool VideoStreamPlaybackHap::_is_paused() const {
    return _paused;
}

double VideoStreamPlaybackHap::_get_length() const {
    return _total_duration;
}

double VideoStreamPlaybackHap::_get_playback_position() const {
    return _time;
}

void VideoStreamPlaybackHap::_seek(double p_time) {
    _time = CLAMP(p_time, 0.0, _total_duration);
    _current_frame = -1;
}

void VideoStreamPlaybackHap::_update(double p_delta) {
    if (!_playing || _paused || !_file.is_valid()) {
        return;
    }

    _time += p_delta;

    if (_time >= _total_duration) {
        _stop();
        return;
    }

    int target = find_frame(_time);
    if (target == _current_frame) {
        return;
    }

    _current_frame = target;
    decode_frame(_current_frame);
}

Ref<Texture2D> VideoStreamPlaybackHap::_get_texture() const {
    return _texture;
}

// ---------------------------------------------------------------------------
// GDExtension binding
// ---------------------------------------------------------------------------

bool VideoStreamPlaybackHap::is_ycocg() const {
    return _video_hap_format == HapTextureFormat_YCoCg_DXT5;
}

void VideoStreamPlaybackHap::_bind_methods() {
    ClassDB::bind_method(D_METHOD("is_ycocg"), &VideoStreamPlaybackHap::is_ycocg);
}
