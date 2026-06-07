/**
 * Libretro C++ Core: Spheroid
 * -----------------------------------------------------------------------------
 * Target Resolution: 320x240 (TBDR Software Renderer)
 * Architecture: QuickJS JavaScript Runtime
 * Interfacing: Shared 32MB ArrayBuffer and Native function bindings
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <vector>
#include <string>

#include "gpu.hpp"
#include "script.hpp"
#include "vfs.hpp"
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
constexpr unsigned SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE / FPS;

// RAM Configuration (32 MB)
const uint32_t RAM_SIZE = 32 * 1024 * 1024;
static uint8_t* console_ram = nullptr;

// VRAM Offsets (Must match game.js!)
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
static VFSManager vfs; // <--- NEW VFS MANAGER

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
}

RETRO_API void retro_init(void) {
   const char *sys_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir)
      snprintf(system_directory, sizeof(system_directory), "%s", sys_dir);

   const char *sav_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sav_dir) && sav_dir)
      snprintf(save_directory, sizeof(save_directory), "%s", sav_dir);

   // 1. Initialize Virtual File System via Libretro
   struct retro_vfs_interface_info vfs_info = { 1, nullptr };
   environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info);
   vfs.init(vfs_info.iface, sav_dir);

   // 2. Allocate 32MB RAM
   console_ram = (uint8_t*)calloc(1, RAM_SIZE);

   // 3. Initialize GPU
   gpu.init(SCREEN_WIDTH, SCREEN_HEIGHT);
   gpu.set_ram_pointer(console_ram);

   // 4. Initialize QuickJS Engine (Pass the VFS Manager to it!)
   if (!script.init(console_ram, RAM_SIZE, &vfs, log_cb)) {
       if (log_cb) log_cb(RETRO_LOG_ERROR, "Failed to initialize QuickJS Engine!\n");
   }
}

RETRO_API void retro_deinit(void) {
   gpu.shutdown();
   script.shutdown();
   vfs.close_all();
   if (console_ram) { free(console_ram); console_ram = nullptr; }
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
   memset(info, 0, sizeof(*info));
   info->library_name     = "Spheroid (JS)";
   info->library_version  = "0.4";
   info->need_fullpath    = true; 
   info->valid_extensions = "js|zip|spheroid"; // Allow zips/folders
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
}

static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t mod) {}

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
    if (!info || !info->path) return false;

    snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;

    // 1. Mount the game directory in the VFS
    vfs.mount_game(retro_game_path);

    // 2. Load "boot.js" via the VFS! (LÖVE2D style boot sequence)
    std::vector<char> js_code;
    int fd = vfs.open("boot.js", "r");
    
    if (fd >= 0) {
        vfs.seek(fd, 0, 2); // SEEK_END
        int64_t size = vfs.tell(fd);
        vfs.seek(fd, 0, 0); // SEEK_SET

        js_code.resize(size + 1, '\0');
        vfs.read(fd, js_code.data(), size);
        vfs.close(fd);
    } 
    else if (info->data && info->size > 0) {
        // Fallback: If they literally loaded a direct JS file that isn't named boot.js
        js_code.assign((const char*)info->data, (const char*)info->data + info->size);
        js_code.push_back('\0');
    } 
    else {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "Failed to load boot.js!\n");
        return false;
    }

    // 3. Load and evaluate the script (We no longer need to pass the filepath 
    //    because the VFS manager handles directory scoping internally now)
        if (!script.load_game(js_code.data(), strlen(js_code.data()))) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "Script failed to evaluate.\n");
        return false;
    }

    struct retro_keyboard_callback kb_cb = { keyboard_cb };
    environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_cb);

    retro_reset();

    // 4. Fire the JS init() callback
    script.call_init();

    return true;
}

RETRO_API void retro_run(void) {
   if (input_poll_cb) input_poll_cb();
   
   // Poll all 16 buttons for up to 4 players
   uint16_t pad_states[4] = {0};
   if (input_state_cb) {
       for (int port = 0; port < 4; port++) {
           for (int btn = 0; btn < 16; btn++) {
               // RETRO_DEVICE_JOYPAD = 1
               if (input_state_cb(port, 1, 0, btn)) {
                   pad_states[port] |= (1 << btn);
               }
           }
       }
   }
   
   // Push the input state to the JS Engine
   script.update_inputs(pad_states);

   core_state.frame_count++;

   script.call_update();

   gpu.execute_display_list(VRAM_LIST_OFFSET);
   video_cb(gpu.get_framebuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint32_t));

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
   return sizeof(CoreState) + sizeof(sram) + RAM_SIZE; 
}

RETRO_API bool retro_serialize(void *data, size_t size) {
   if (size < retro_serialize_size()) return false;
   uint8_t *ptr = (uint8_t*)data;
   
   memcpy(ptr, &core_state, sizeof(CoreState)); ptr += sizeof(CoreState);
   memcpy(ptr, sram, sizeof(sram)); ptr += sizeof(sram);
   memcpy(ptr, console_ram, RAM_SIZE); 
   
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
   if (size < retro_serialize_size()) return false;
   const uint8_t *ptr = (const uint8_t*)data;
   
   memcpy(&core_state, ptr, sizeof(CoreState)); ptr += sizeof(CoreState);
   memcpy(sram, ptr, sizeof(sram)); ptr += sizeof(sram);
   memcpy(console_ram, ptr, RAM_SIZE); 
   
   return true;
}

RETRO_API void *retro_get_memory_data(unsigned id) { return (id == RETRO_MEMORY_SAVE_RAM) ? sram : nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return (id == RETRO_MEMORY_SAVE_RAM) ? sizeof(sram) : 0; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

} // extern "C"