#pragma once

#include <cstdint>
#include <cstddef>
#include "miniaudio.h"
#include <libretro.h>

class SpheroidAPU {
public:
    // =========================================================================
    // Register Map Constants
    // =========================================================================
    
    // Per-Channel Registers (0x00 - 0x0D)
    static constexpr uint32_t REG_START_ADDR   = 0x00;
    static constexpr uint32_t REG_END_ADDR     = 0x01;
    static constexpr uint32_t REG_LOOP_ADDR    = 0x02; // (Not used natively by miniaudio buffer yet, defaults to start)
    static constexpr uint32_t REG_LOOP_ENABLE  = 0x03;
    static constexpr uint32_t REG_PITCH        = 0x04; // 0x1000 = 1.0x
    static constexpr uint32_t REG_VOL_LEFT     = 0x05; // 0 - 0x3FFF
    static constexpr uint32_t REG_VOL_RIGHT    = 0x06; // 0 - 0x3FFF
    static constexpr uint32_t REG_ADSR_ATTACK  = 0x07; // Attack time in MS
    static constexpr uint32_t REG_ADSR_DECAY   = 0x08; // (Handled via miniaudio fades)
    static constexpr uint32_t REG_ADSR_SUSTAIN = 0x09; // (Handled via miniaudio fades)
    static constexpr uint32_t REG_ADSR_RELEASE = 0x0A; // Release time in MS
    static constexpr uint32_t REG_DELAY_SEND   = 0x0B; // (Reserved for future reverb node)
    static constexpr uint32_t REG_CHANNELS     = 0x0C; // 1 = Mono, 2 = Stereo
    static constexpr uint32_t REG_PLAY_POS     = 0x0D; // [Read-Only] Absolute RAM Addr
    static constexpr uint32_t REG_ENV_LEVEL    = 0x0E; // [Read-Only] 0 - 0x3FFF

    // Global Registers
    static constexpr uint32_t REG_GLOBAL_KEYON       = 0x100;
    static constexpr uint32_t REG_GLOBAL_KEYOFF      = 0x101;
    static constexpr uint32_t REG_GLOBAL_DELAY_LEN   = 0x102;
    static constexpr uint32_t REG_GLOBAL_DELAY_FB    = 0x103;
    static constexpr uint32_t REG_GLOBAL_DELAY_VOL_L = 0x104;
    static constexpr uint32_t REG_GLOBAL_DELAY_VOL_R = 0x105;
    static constexpr uint32_t REG_GLOBAL_STATUS      = 0x106; // [Read-Only] Active mask

    // =========================================================================

    SpheroidAPU() = default;

    void init(uint8_t* ram, size_t ram_size, uint32_t sample_rate, retro_log_printf_t log_cb);

    // Memory-mapped Register API
    void write(uint32_t ch, uint32_t reg, uint32_t val);
    uint32_t read(uint32_t ch, uint32_t reg);
    void writeGlobal(uint32_t reg, uint32_t val);
    uint32_t readGlobal(uint32_t reg);

    // Audio mixing callback (Called by retro_run)
    void render_frame(int16_t* out_buffer, size_t num_frames);
	
	void shutdown();
	
private:
    static constexpr int NUM_VOICES = 24;

    // The Hardware Register State (What JS writes to)
    struct VoiceRegs {
        uint32_t start_addr = 0;
        uint32_t end_addr = 0;
        bool loop_enable = false;
        uint32_t pitch = 0x1000;
        uint32_t channels = 1;

        float vol_l = 0.0f;
        float vol_r = 0.0f;

        uint32_t attack_ms = 0;
        uint32_t release_ms = 0;
    };

    VoiceRegs regs[NUM_VOICES];

    // Miniaudio Subsystem
    ma_engine engine;
    ma_audio_buffer audio_buffers[NUM_VOICES];
    ma_sound sounds[NUM_VOICES];
    bool sound_initialized[NUM_VOICES] = {false};

    uint8_t* system_ram = nullptr;
    size_t system_ram_size = 0;
    uint32_t out_sample_rate = 44100;

	retro_log_printf_t logger = nullptr;

    void key_on(uint32_t mask);
    void key_off(uint32_t mask);
};