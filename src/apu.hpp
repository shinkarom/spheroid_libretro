#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include "vfs.hpp"

class SpheroidAPU {
public:
    SpheroidAPU() = default;
    ~SpheroidAPU() = default;

    void init(VFSManager* vfs_mgr, uint32_t sample_rate);
    void shutdown();

    int load_sound(const char* filepath);
    void unload_sound(int sound_id);

    int play(int sound_id, float volume, float pitch, float pan, bool loop);
    void stop(int voice_id);
    void stop_all();

    void set_volume(int voice_id, float volume);
    void set_pitch(int voice_id, float pitch);
    void set_pan(int voice_id, float pan);

    void render_frame(int16_t* out_buffer, size_t num_frames);

private:
    static constexpr int NUM_VOICES = 24;

    VFSManager* vfs = nullptr;
    uint32_t out_sample_rate = 44100;

    struct SoundBuffer {
        std::vector<int16_t> pcm_data;
    };

    struct Voice {
        bool active = false;
        int sound_id = -1;
        float position = 0.0f; // Floating point for pitch interpolation
        float pitch = 1.0f;
        float vol_l = 1.0f;
        float vol_r = 1.0f;
        bool loop = false;
    };

    Voice voices[NUM_VOICES];
    std::unordered_map<int, SoundBuffer> sound_bank;
    int next_sound_id = 1;

    // Tiny custom WAV parser
    bool parse_wav(const std::vector<uint8_t>& file_data, std::vector<int16_t>& out_pcm);
};