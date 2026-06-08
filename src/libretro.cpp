/**
 * Libretro C++ Core: Spheroid
 * -----------------------------------------------------------------------------
 * Target Resolution: 480x270 (16:9 TBDR Software Renderer)
 * Architecture: QuickJS JavaScript Runtime
 * Interfacing: Native JS Bindings (Resource Manager & Command Queue)
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
#include "apu.hpp"
#include "HandmadeMath.h"
#include "libretro.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern "C" {

// =============================================================================
// Core Configuration & Globals
// =============================================================================

constexpr unsigned SCREEN_WIDTH  = 480;
constexpr unsigned SCREEN_HEIGHT = 270;
constexpr float ASPECT_RATIO     = SCREEN_WIDTH * 1.0f / SCREEN_HEIGHT;

constexpr unsigned AUDIO_SAMPLE_RATE = 44100;
constexpr unsigned FPS = 60;
constexpr unsigned SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE / FPS;

// JS/System RAM Configuration (16 MB is plenty now that GPU manages its own VRAM)
const uint32_t RAM_SIZE = 16 * 1024 * 1024;
static uint8_t* console_ram = nullptr;

// Libretro / Frontend Globals
static struct retro_log_callback logging;
static retro_log_printf_t log_cb;
char system_directory[4096]; 
char save_directory[4096];   
char retro_game_path[4096];  

// Emulator Subsystems
static SpheroidGPU gpu;
static SpheroidScript script;
static VFSManager vfs; 
static SpheroidAPU apu;

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
   };

   static const struct retro_controller_info ports[] = { { controllers, 2 }, { nullptr, 0 } };
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

RETRO_API void retro_init(void) {
   const char *sys_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir)
      snprintf(system_directory, sizeof(system_directory), "%s", sys_dir);

   const char *sav_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sav_dir) && sav_dir)
      snprintf(save_directory, sizeof(save_directory), "%s", sav_dir);

   // 1. Initialize Virtual File System
   struct retro_vfs_interface_info vfs_info = { 1, nullptr };
   environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info);
   vfs.init(vfs_info.iface, sav_dir);

   // 2. Allocate System RAM (JS Heap + APU Data)
   console_ram = (uint8_t*)calloc(1, RAM_SIZE);

   // 3. Initialize GPU
   gpu.init(SCREEN_WIDTH, SCREEN_HEIGHT);
	
   // 4. Initialize APU
   apu.init(&vfs, AUDIO_SAMPLE_RATE);
	
   // 5. Initialize QuickJS Engine
   // NOTE: We now pass the GPU pointer so QuickJS can bind Spheroid.GPU functions!
   if (!script.init(console_ram, RAM_SIZE, &vfs, &apu, &gpu, log_cb)) {
       if (log_cb) log_cb(RETRO_LOG_ERROR, "Failed to initialize QuickJS Engine!\n");
   }
}

RETRO_API void retro_deinit(void) {
   gpu.shutdown();
   script.shutdown();
   vfs.close_all();
   apu.shutdown(); 
   if (console_ram) { free(console_ram); console_ram = nullptr; }
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
   memset(info, 0, sizeof(*info));
   info->library_name     = "Spheroid";
   info->library_version  = "1.0";
   info->need_fullpath    = true; 
   info->valid_extensions = "js|zip|spheroid"; 
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

    vfs.mount_game(retro_game_path);

    std::vector<char> js_code;
    int fd = vfs.open("boot.js", "r");
    
    if (fd >= 0) {
        vfs.seek(fd, 0, 2); 
        int64_t size = vfs.tell(fd);
        vfs.seek(fd, 0, 0); 

        js_code.resize(size + 1, '\0');
        
        // Safer read
        int64_t bytes_read = vfs.read(fd, js_code.data(), size);
        js_code[bytes_read] = '\0'; 
        
        vfs.close(fd);
    } 
    else if (info->data && info->size > 0) {
        js_code.assign((const char*)info->data, (const char*)info->data + info->size);
        js_code.push_back('\0');
    } 
    else {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "Failed to load boot.js!\n");
        return false;
    }

    // Pass the actual length of the string to avoid premature termination issues
    if (!script.load_game(js_code.data(), js_code.size() - 1)) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "Script failed to evaluate.\n");
        return false;
    }

    struct retro_keyboard_callback kb_cb = { keyboard_cb };
    environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_cb);

    retro_reset();

    // Fire the JS init() callback (This is where JS will load meshes/textures into the GPU)
    script.call_init();

    return true;
}

RETRO_API void retro_run(void) {
   if (input_poll_cb) input_poll_cb();
   
   uint16_t pad_states[4] = {0};
   if (input_state_cb) {
       for (int port = 0; port < 4; port++) {
           for (int btn = 0; btn < 16; btn++) {
               if (input_state_cb(port, 1, 0, btn)) {
                   pad_states[port] |= (1 << btn);
               }
           }
       }
   }
   
   script.update_inputs(pad_states);

   core_state.frame_count++;

   // 1. JS Execution: Builds the RenderCommand Queue internally
   script.call_update();

   // 2. GPU Execution: Flushes queue to the TBDR worker threads
   gpu.flush_command_queue();
   
   // 3. Output Framebuffer to Libretro
   video_cb(gpu.get_framebuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint32_t));

   // 4. Audio Processing
   int16_t audio_buf[SAMPLES_PER_FRAME * 2];
   apu.render_frame(audio_buf, SAMPLES_PER_FRAME); 
   audio_batch_cb(audio_buf, SAMPLES_PER_FRAME);
}

RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

// =============================================================================
// Serialization (Savestates & Rewind)
// =============================================================================
// TODO: Full savestate support now requires serializing the GPU's internal 
// std::vector for Textures and Meshes, as they are no longer in system RAM.

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