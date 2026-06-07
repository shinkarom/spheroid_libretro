/**
 * Libretro C++ Core: Spheroid (QuickJS Engine)
 * -----------------------------------------------------------------------------
 * Target Resolution: 320x240 (TBDR Software Renderer)
 * Architecture: QuickJS JavaScript Runtime
 * Interfacing: Shared 32MB ArrayBuffer and Native function bindings
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#include "gpu.hpp"
#include "script.hpp"
#include "HandmadeMath.h"
#include "libretro.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern "C" {

// =============================================================================
// Core Configuration & Globals
// =============================================================================

constexpr unsigned SCREEN_WIDTH  = 320;
constexpr unsigned SCREEN_HEIGHT = 240;
constexpr float ASPECT_RATIO     = SCREEN_WIDTH * 1.0f / SCREEN_HEIGHT;

constexpr unsigned AUDIO_SAMPLE_RATE = 44100;
constexpr unsigned FPS = 60;
constexpr unsigned SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE / FPS; // 735 samples

// RAM Configuration (Reduced to 32 MB for JS VRAM/Data)
const uint32_t RAM_SIZE = 32 * 1024 * 1024;
static uint8_t* console_ram = nullptr;

// VRAM Offsets (Match exactly with JS definitions)
constexpr uint32_t VRAM_LIST_OFFSET    = 0x00000000; 
constexpr uint32_t VRAM_VERTEX_OFFSET  = 0x00800000; 
constexpr uint32_t VRAM_TEXTURE_OFFSET = 0x01000000;
constexpr uint32_t VRAM_INDEX_OFFSET   = 0x01800000;

// Libretro / Frontend Globals
static struct retro_log_callback logging;
static retro_log_printf_t log_cb;
char system_directory[4096]; 
char save_directory[4096];   
char retro_game_path[4096];  

// Emulator Subsystems
static SpheroidGPU gpu;
static SpheroidScript script;

struct CoreState {
   uint32_t frame_count;
   double internal_timer;
};
static CoreState core_state = {0, 0.0};
static uint8_t sram[0x2000]; // 8KB Battery Backup RAM

// Callbacks
static retro_environment_t environ_cb;           
static retro_video_refresh_t video_cb;           
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;         
static retro_input_state_t input_state_cb;       

// =============================================================================
// Utilities & Audio
// =============================================================================
static void fallback_log(enum retro_log_level level, const char *fmt, ...) {
   (void)level;
   va_list va; va_start(va, fmt); vfprintf(stderr, fmt, va); va_end(va);
}

static void generate_audio(int16_t *buffer, size_t num_frames) {
   static double phase = 0.0;
   const double phase_increment = (2.0 * M_PI * 440.0) / AUDIO_SAMPLE_RATE;

   for (size_t i = 0; i < num_frames; i++) {
      int16_t sample = (int16_t)(0x800 * std::sin(phase));
      buffer[i * 2 + 0] = sample; 
      buffer[i * 2 + 1] = sample; 
      phase += phase_increment;
      if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
   }
}

// =============================================================================
// Libretro Core Lifecycle
// =============================================================================
RETRO_API void retro_set_environment(retro_environment_t cb) {
   environ_cb = cb;
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
   else log_cb = fallback_log;

   bool no_rom = false;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

   static const struct retro_controller_description controllers[] = {
      { "RetroPad", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) },
      { "RetroPad w/ Analog", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0) },
      { "Mouse", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0) },
      { "Keyboard", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_KEYBOARD, 0) },
   };

   static const struct retro_controller_info ports[] = { { controllers, 4 }, { nullptr, 0 } };
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

   struct retro_variable variables[] = {
      { "spheroid_tbdr_threads", "Renderer Threads; 1|2|4|8" },
      { nullptr, nullptr },
   };
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

RETRO_API void retro_init(void) {
   const char *sys_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir)
      snprintf(system_directory, sizeof(system_directory), "%s", sys_dir);

   const char *sav_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sav_dir) && sav_dir)
      snprintf(save_directory, sizeof(save_directory), "%s", sav_dir);

   // Allocate 32MB RAM
   console_ram = (uint8_t*)calloc(1, RAM_SIZE);

   // Initialize GPU
   gpu.init(SCREEN_WIDTH, SCREEN_HEIGHT);
   gpu.set_ram_pointer(console_ram);

   // Initialize QuickJS Scripting Engine
   if (!script.init(console_ram, RAM_SIZE, log_cb)) {
       if (log_cb) log_cb(RETRO_LOG_ERROR, "Failed to initialize QuickJS Engine!\n");
   }
}

RETRO_API void retro_deinit(void) {
   gpu.shutdown();
   script.shutdown();
   if (console_ram) { free(console_ram); console_ram = nullptr; }
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
   memset(info, 0, sizeof(*info));
   info->library_name     = "Spheroid (JS)";
   info->library_version  = "0.3";
   info->need_fullpath    = true; 
   info->valid_extensions = "js"; // Look for JavaScript files now!
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
   info->geometry.base_width   = SCREEN_WIDTH;
   info->geometry.base_height  = SCREEN_HEIGHT;
   info->geometry.max_width    = SCREEN_WIDTH;
   info->geometry.max_height   = SCREEN_HEIGHT;
   info->geometry.aspect_ratio = ASPECT_RATIO;
   info->timing.fps            = FPS;
   info->timing.sample_rate    = AUDIO_SAMPLE_RATE;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }

RETRO_API void retro_reset(void) {
   core_state.frame_count = 0;
   core_state.internal_timer = 0.0;
   // Note: In a JS context, we might want to reload the script here,
   // or call a dedicated JS `reset()` function in the future.
}

static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t mod) {
   // Hardware Keyboard hook
}

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
    if (!info || !info->path) return false;

    snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;

    // Load JS File Source Code
    FILE *rom = fopen(retro_game_path, "rb");
    if (!rom) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "Failed to open JS file: %s\n", retro_game_path);
        return false;
    }
    
    fseek(rom, 0, SEEK_END);
    size_t file_size = ftell(rom);
    fseek(rom, 0, SEEK_SET);

    std::vector<char> js_code(file_size + 1, '\0'); // +1 for null terminator
    fread(js_code.data(), 1, file_size, rom);
    fclose(rom);

    // Load and evaluate the script
    if (!script.load_game(js_code.data(), file_size)) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "Script failed to load or evaluate.\n");
        return false;
    }

    struct retro_keyboard_callback kb_cb = { keyboard_cb };
    environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_cb);

    retro_reset();

    // Fire the JS init() callback once to let the game set up VRAM/Textures
    script.call_init();

    return true;
}

RETRO_API void retro_run(void) {
   if (input_poll_cb) input_poll_cb();
   
   core_state.frame_count++;

   // 1. Run Game Logic (JS calls System.print, writes to System.RAM ArrayBuffer)
   script.call_update();

   // 2. Render Graphics (GPU reads from the C++ console_ram that JS just modified!)
   gpu.execute_display_list(VRAM_LIST_OFFSET);
   video_cb(gpu.get_framebuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint32_t));

   // 3. Audio Output
   int16_t audio_buf[SAMPLES_PER_FRAME * 2];
   generate_audio(audio_buf, SAMPLES_PER_FRAME); 
   audio_batch_cb(audio_buf, SAMPLES_PER_FRAME);
}

RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

// =============================================================================
// Serialization (Savestates & Rewind)
// =============================================================================

RETRO_API size_t retro_serialize_size(void) { 
   // We only need to save the C++ state, SRAM, and the 32MB ArrayBuffer.
   // (JS Engine internal state is not saved—the JS game must store logic in RAM!)
   return sizeof(CoreState) + sizeof(sram) + RAM_SIZE; 
}

RETRO_API bool retro_serialize(void *data, size_t size) {
   if (size < retro_serialize_size()) return false;
   uint8_t *ptr = (uint8_t*)data;
   
   memcpy(ptr, &core_state, sizeof(CoreState)); ptr += sizeof(CoreState);
   memcpy(ptr, sram, sizeof(sram)); ptr += sizeof(sram);
   
   // Save 32MB System RAM (Contains textures, VRAM, and whatever JS put there)
   memcpy(ptr, console_ram, RAM_SIZE); 
   
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
   if (size < retro_serialize_size()) return false;
   const uint8_t *ptr = (const uint8_t*)data;
   
   memcpy(&core_state, ptr, sizeof(CoreState)); ptr += sizeof(CoreState);
   memcpy(sram, ptr, sizeof(sram)); ptr += sizeof(sram);
   
   // Restore 32MB System RAM
   memcpy(console_ram, ptr, RAM_SIZE); 
   
   return true;
}

RETRO_API void *retro_get_memory_data(unsigned id) { return (id == RETRO_MEMORY_SAVE_RAM) ? sram : nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return (id == RETRO_MEMORY_SAVE_RAM) ? sizeof(sram) : 0; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

} // extern "C"