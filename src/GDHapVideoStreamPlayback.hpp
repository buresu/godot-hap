#pragma once

#include <cstdio>
#include <vector>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/video_stream_playback.hpp>

#define MP4D_64BIT_SUPPORTED 1
#include "minimp4.h"

#include "hap.h"

using namespace godot;

class GDHapVideoStreamPlayback : public VideoStreamPlayback {
    GDCLASS(GDHapVideoStreamPlayback, VideoStreamPlayback)

    struct FrameInfo {
        double time;
        double duration;
        MP4D_file_offset_t offset;
        unsigned size;
    };

    FILE *file = nullptr;
    MP4D_demux_t mp4 = {};
    int video_track = -1;

    int width = 0;
    int height = 0;
    unsigned int frame_count = 0;
    double total_duration = 0.0;

    std::vector<FrameInfo> frames;
    std::vector<uint8_t> read_buf;
    std::vector<uint8_t> decode_buf;

    Ref<ImageTexture> texture;

    double time = 0.0;
    int current_frame = -1;
    bool playing = false;
    bool paused = false;

    static int read_callback(int64_t offset, void *buffer, size_t size, void *token);
    static void hap_decode_callback(HapDecodeWorkFunction function, void *p, unsigned int count, void *info);

    static Image::Format hap_to_godot_format(unsigned int hap_format);
    static size_t dxt_buffer_size(unsigned int hap_format, int w, int h);

    int find_frame(double p_time) const;
    void decode_frame(int index);
    bool read_hap_dimensions();

protected:
    static void _bind_methods();

public:
    ~GDHapVideoStreamPlayback();

    void open(const String &p_path);

    void _play() override;
    void _stop() override;
    bool _is_playing() const override;
    void _set_paused(bool p_paused) override;
    bool _is_paused() const override;
    double _get_length() const override;
    double _get_playback_position() const override;
    void _seek(double p_time) override;
    void _update(double p_delta) override;
    Ref<Texture2D> _get_texture() const override;
    int _get_channels() const override { return 0; }
    int32_t _get_mix_rate() const override { return 0; }
};
