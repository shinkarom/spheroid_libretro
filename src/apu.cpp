#include "apu.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

void SpheroidAPU::shutdown() {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (sound_initialized[i]) {
            ma_sound_uninit(&sounds[i]);
            ma_audio_buffer_uninit(&audio_buffers[i]);
        }
    }
    ma_engine_uninit(&engine);
}

void SpheroidAPU::init(uint8_t* ram, size_t ram_size, uint32_t sample_rate, retro_log_printf_t log_cb) {
	logger = log_cb;
    system_ram = ram;
    system_ram_size = ram_size;
    out_sample_rate = sample_rate;

    // Initialize Miniaudio strictly as an in-memory mixer (no OS audio devices)
    ma_engine_config engConfig = ma_engine_config_init();
    engConfig.noDevice = MA_TRUE;           // We will pull the frames manually
    engConfig.channels = 2;                 // Force Stereo output
    engConfig.sampleRate = out_sample_rate; // Match Libretro's 44100Hz

    if (ma_engine_init(&engConfig, &engine) != MA_SUCCESS) {
        if (logger) logger(RETRO_LOG_ERROR, "[APU] Failed to initialize Miniaudio Engine!\n");
    }
}

// =============================================================================
// Register API
// =============================================================================

void SpheroidAPU::write(uint32_t ch, uint32_t reg, uint32_t val) {
    if (ch >= NUM_VOICES) return;
    VoiceRegs& v = regs[ch];

    switch (reg) {
        case REG_START_ADDR:  v.start_addr = val; break;
        case REG_END_ADDR:    v.end_addr = val; break;
        case REG_LOOP_ENABLE: v.loop_enable = (val != 0); break;
        case REG_PITCH:       v.pitch = val; break;
        case REG_CHANNELS:    v.channels = (val == 2) ? 2 : 1; break;
        
        // Volume conversions (0x3FFF -> 1.0f)
        case REG_VOL_LEFT:    v.vol_l = (val & 0x3FFF) / 16383.0f; break;
        case REG_VOL_RIGHT:   v.vol_r = (val & 0x3FFF) / 16383.0f; break;
        
        // ADSR Millisecond times
        case REG_ADSR_ATTACK:  v.attack_ms = val; break; 
        case REG_ADSR_RELEASE: v.release_ms = val; break;
    }

    // Apply Live Pitch/Volume changes if the sound is already playing
    if (sound_initialized[ch] && ma_sound_is_playing(&sounds[ch])) {
        if (reg == REG_PITCH) {
            ma_sound_set_pitch(&sounds[ch], v.pitch / 4096.0f);
        } else if (reg == REG_VOL_LEFT || reg == REG_VOL_RIGHT) {
            float max_vol = std::max(v.vol_l, v.vol_r);
            float pan = (max_vol == 0.0f) ? 0.0f : ((v.vol_r - v.vol_l) / max_vol);
            ma_sound_set_volume(&sounds[ch], max_vol);
            ma_sound_set_pan(&sounds[ch], pan);
        }
    }
}

uint32_t SpheroidAPU::read(uint32_t ch, uint32_t reg) {
    if (ch >= NUM_VOICES) return 0;
    VoiceRegs& v = regs[ch];

    switch (reg) {
        case REG_START_ADDR:  return v.start_addr;
        case REG_END_ADDR:    return v.end_addr;
        case REG_LOOP_ENABLE: return v.loop_enable ? 1 : 0;
        case REG_PITCH:       return v.pitch;
        case REG_CHANNELS:    return v.channels;
        case REG_VOL_LEFT:    return (uint32_t)(v.vol_l * 16383.0f);
        case REG_VOL_RIGHT:   return (uint32_t)(v.vol_r * 16383.0f);
        case REG_ADSR_ATTACK: return v.attack_ms;
        case REG_ADSR_RELEASE:return v.release_ms;
        
        // Ask miniaudio exactly where the play cursor is in memory
        case REG_PLAY_POS: {
            if (sound_initialized[ch]) {
                ma_uint64 cursor_frame = 0;
                ma_sound_get_cursor_in_pcm_frames(&sounds[ch], &cursor_frame);
                return v.start_addr + (uint32_t)(cursor_frame * 2 * v.channels); // 2 bytes per sample
            }
            return v.start_addr;
        }
        
        default: return 0;
    }
}

void SpheroidAPU::writeGlobal(uint32_t reg, uint32_t val) {
    if (reg == REG_GLOBAL_KEYON) key_on(val);
    if (reg == REG_GLOBAL_KEYOFF) key_off(val);
}

uint32_t SpheroidAPU::readGlobal(uint32_t reg) {
    if (reg == REG_GLOBAL_STATUS) {
        uint32_t mask = 0;
        for (int i = 0; i < NUM_VOICES; i++) {
            if (sound_initialized[i] && ma_sound_is_playing(&sounds[i])) {
                mask |= (1 << i);
            }
        }
        return mask;
    }
    return 0;
}

// =============================================================================
// Miniaudio Hardware Mapping
// =============================================================================

void SpheroidAPU::key_on(uint32_t mask) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (mask & (1 << i)) {
            VoiceRegs& v = regs[i];

            // 1. Cleanup old sound safely
            if (sound_initialized[i]) {
                ma_sound_stop(&sounds[i]); // <--- MUST STOP BEFORE UNINIT TO PREVENT DEADLOCKS
                ma_sound_uninit(&sounds[i]);
                ma_audio_buffer_uninit(&audio_buffers[i]);
                sound_initialized[i] = false;
            }

            // Safety check
            if (v.end_addr <= v.start_addr || v.end_addr > system_ram_size) continue;

            // 2. Point Miniaudio's Buffer directly at the C++ RAM
            size_t size_in_bytes = v.end_addr - v.start_addr;
            ma_uint64 frame_count = size_in_bytes / (2 * v.channels); 

            ma_audio_buffer_config bufConfig = ma_audio_buffer_config_init(
                ma_format_s16, v.channels, frame_count, system_ram + v.start_addr, NULL
            );

            if (ma_audio_buffer_init(&bufConfig, &audio_buffers[i]) != MA_SUCCESS) continue;

            // 3. Initialize the Sound Node
            if (ma_sound_init_from_data_source(&engine, &audio_buffers[i], 0, NULL, &sounds[i]) != MA_SUCCESS) {
                ma_audio_buffer_uninit(&audio_buffers[i]);
                continue;
            }

            sound_initialized[i] = true;

            // 4. Apply Hardware Registers
            ma_sound_set_pitch(&sounds[i], v.pitch / 4096.0f);
            ma_sound_set_looping(&sounds[i], v.loop_enable ? MA_TRUE : MA_FALSE);

            float max_vol = std::max(v.vol_l, v.vol_r);
            float pan = (max_vol == 0.0f) ? 0.0f : ((v.vol_r - v.vol_l) / max_vol);
            
            if (v.attack_ms > 0) {
                ma_sound_set_volume(&sounds[i], 0.0f); 
                ma_sound_set_fade_in_milliseconds(&sounds[i], 0.0f, max_vol, v.attack_ms);
            } else {
                ma_sound_set_volume(&sounds[i], max_vol);
            }
            
            ma_sound_set_pan(&sounds[i], pan);

            // STRIKE!
            ma_sound_start(&sounds[i]);
        }
    }
}

void SpheroidAPU::key_off(uint32_t mask) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if ((mask & (1 << i)) && sound_initialized[i]) {
            // SAFE FADE OUT: Only fade if it is actively playing and HASN'T reached the end!
            if (ma_sound_is_playing(&sounds[i]) && !ma_sound_at_end(&sounds[i])) {
                if (regs[i].release_ms > 0) {
                    ma_sound_stop_with_fade_in_milliseconds(&sounds[i], regs[i].release_ms);
                } else {
                    ma_sound_stop(&sounds[i]);
                }
            } else {
                // If it already finished naturally, just ensure it's stopped.
                ma_sound_stop(&sounds[i]);
            }
        }
    }
}

// =============================================================================
// Mixer Output
// =============================================================================

void SpheroidAPU::render_frame(int16_t* out_buffer, size_t num_frames) {
    // We allocate a local array (735 * 2 = 1470 floats) which easily fits on the stack
    // (Libretro passes a maximum of 4096 frames, so 8192 is incredibly safe).
    float f32_mix[8192] = {0.0f}; 
    
    // Mix all 24 voices
    ma_engine_read_pcm_frames(&engine, f32_mix, num_frames, NULL);

    // Convert mixed floats back to 16-bit PCM for Libretro
    for (size_t i = 0; i < num_frames * 2; i++) {
        float sample = f32_mix[i];
        sample = std::clamp(sample, -1.0f, 1.0f);
        out_buffer[i] = (int16_t)(sample * 32767.0f);
    }
}