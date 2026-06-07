#include "apu.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

void SpheroidAPU::init(VFSManager* vfs_mgr, uint32_t sample_rate) {
    vfs = vfs_mgr;
    out_sample_rate = sample_rate;
}

void SpheroidAPU::shutdown() {
    stop_all();
    sound_bank.clear();
}

// =============================================================================
// Custom WAV Parser
// =============================================================================
bool SpheroidAPU::parse_wav(const std::vector<uint8_t>& file_data, std::vector<int16_t>& out_pcm) {
    // A standard WAV header is 44 bytes. We do a very basic check.
    if (file_data.size() < 44) return false;
    if (memcmp(file_data.data(), "RIFF", 4) != 0) return false;
    
    // Find the "data" chunk (usually at byte 36, but we scan to be safe)
    size_t data_offset = 12;
    while (data_offset < file_data.size() - 8) {
        if (memcmp(&file_data[data_offset], "data", 4) == 0) {
            uint32_t data_size = *(uint32_t*)(&file_data[data_offset + 4]);
            size_t pcm_start = data_offset + 8;
            
            if (pcm_start + data_size <= file_data.size()) {
                size_t num_samples = data_size / 2; // 16-bit = 2 bytes per sample
                out_pcm.resize(num_samples);
                memcpy(out_pcm.data(), &file_data[pcm_start], data_size);
                return true;
            }
            break;
        }
        // Skip chunk
        uint32_t chunk_size = *(uint32_t*)(&file_data[data_offset + 4]);
        data_offset += 8 + chunk_size;
    }
    return false;
}

// =============================================================================
// Asset Management
// =============================================================================
int SpheroidAPU::load_sound(const char* filepath) {
    if (!vfs || !filepath) return -1;

    int fd = vfs->open(filepath, "r");
    if (fd < 0) return -1;

    vfs->seek(fd, 0, 2); 
    int64_t size = vfs->tell(fd);
    vfs->seek(fd, 0, 0); 

    std::vector<uint8_t> raw_data(size);
    vfs->read(fd, raw_data.data(), size);
    vfs->close(fd);

    SoundBuffer snd;
    if (!parse_wav(raw_data, snd.pcm_data)) {
        return -1; // Not a valid WAV file
    }

    int id = next_sound_id++;
    sound_bank[id] = std::move(snd);
    return id;
}

void SpheroidAPU::unload_sound(int sound_id) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].active && voices[i].sound_id == sound_id) {
            voices[i].active = false;
        }
    }
    sound_bank.erase(sound_id);
}

// =============================================================================
// Voice Control
// =============================================================================
int SpheroidAPU::play(int sound_id, float volume, float pitch, float pan, bool loop) {
    if (sound_bank.find(sound_id) == sound_bank.end()) return -1;

    int free_voice = -1;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!voices[i].active) {
            free_voice = i;
            break;
        }
    }
    if (free_voice == -1) return -1;

    Voice& v = voices[free_voice];
    v.active = true;
    v.sound_id = sound_id;
    v.position = 0.0f;
    v.pitch = std::max(0.1f, pitch); // Prevent 0 pitch freeze
    v.loop = loop;
    
    set_volume(free_voice, volume);
    set_pan(free_voice, pan);

    return free_voice;
}

void SpheroidAPU::stop(int voice_id) {
    if (voice_id >= 0 && voice_id < NUM_VOICES) voices[voice_id].active = false;
}

void SpheroidAPU::stop_all() {
    for (int i = 0; i < NUM_VOICES; i++) voices[i].active = false;
}

void SpheroidAPU::set_volume(int voice_id, float volume) {
    // Handled in set_pan to maintain stereo balance
}

void SpheroidAPU::set_pitch(int voice_id, float pitch) {
    if (voice_id >= 0 && voice_id < NUM_VOICES) voices[voice_id].pitch = pitch;
}

void SpheroidAPU::set_pan(int voice_id, float pan) {
    if (voice_id >= 0 && voice_id < NUM_VOICES) {
        float p = std::clamp(pan, -1.0f, 1.0f);
        // Constant power panning approximation
        voices[voice_id].vol_l = (p < 0.0f) ? 1.0f : (1.0f - p);
        voices[voice_id].vol_r = (p > 0.0f) ? 1.0f : (1.0f + p);
    }
}

// =============================================================================
// The Custom Software Mixer
// =============================================================================
void SpheroidAPU::render_frame(int16_t* out_buffer, size_t num_frames) {
    for (size_t f = 0; f < num_frames; f++) {
        float mix_l = 0.0f;
        float mix_r = 0.0f;

        for (int i = 0; i < NUM_VOICES; i++) {
            Voice& v = voices[i];
            if (!v.active) continue;

            const auto& pcm = sound_bank[v.sound_id].pcm_data;
            size_t max_pos = pcm.size();

            if (max_pos == 0) {
                v.active = false;
                continue;
            }

            // Nearest-neighbor sampling (Classic PS1/GBA crunchy sound!)
            size_t idx = (size_t)v.position;
            
            if (idx >= max_pos) {
                if (v.loop) {
                    v.position -= max_pos;
                    idx = 0;
                } else {
                    v.active = false;
                    continue;
                }
            }

            float sample = pcm[idx] / 32768.0f;

            mix_l += sample * v.vol_l;
            mix_r += sample * v.vol_r;

            // Advance playback head based on pitch
            v.position += v.pitch;
        }

        // Hard Clipping (Creates awesome distortion if volume is too high)
        mix_l = std::clamp(mix_l, -1.0f, 1.0f);
        mix_r = std::clamp(mix_r, -1.0f, 1.0f);

        out_buffer[f * 2 + 0] = (int16_t)(mix_l * 32767.0f);
        out_buffer[f * 2 + 1] = (int16_t)(mix_r * 32767.0f);
    }
}