#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#define _USE_MATH_DEFINES
#include <math.h>

#if defined(_WIN32) && !defined(_XBOX)
#include <windows.h>
#endif

#include "libretro.h"
#include <unicorn/unicorn.h>

extern "C" {

// Standard generic retro resolution
constexpr auto screenWidth = 320;
constexpr auto screenHeight = 240;
constexpr auto screenTotalPixels = screenWidth * screenHeight;
constexpr auto audioSampleRate = 44100;
constexpr auto fps = 60;
constexpr auto samplesPerFrame = audioSampleRate / fps;

static uint32_t *frame_buf = nullptr;
static struct retro_log_callback logging;
static retro_log_printf_t log_cb;
static bool use_audio_cb = false;
static float last_aspect;
static float last_sample_rate;
char retro_base_directory[4096];
char retro_game_path[4096];

static uc_engine *uc = nullptr;

// Callbacks
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

// Helper for generating audio wave safely
static void generate_audio_samples(int16_t* buffer, size_t frames)
{
    static double phase = 0.0;
    const double phase_increment = 2.0 * M_PI * 440.0 / audioSampleRate; // 440Hz tone

    for (size_t i = 0; i < frames; i++)
    {
        int16_t val = (int16_t)(0x800 * sin(phase));
        buffer[i * 2] = val;       // Left channel
        buffer[i * 2 + 1] = val;   // Right channel

        phase += phase_increment;
        if (phase >= 2.0 * M_PI) {
            phase -= 2.0 * M_PI;
        }
    }
}

RETRO_API void retro_init(void)
{
   if (!frame_buf) {
      frame_buf = new uint32_t[screenTotalPixels];
   }
   memset(frame_buf, 0, screenTotalPixels * sizeof(uint32_t));

   const char *dir = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "%s", dir);
   }

   // Initialize Unicorn Engine (Change ARCH and MODE as needed for your target)
   uc_err err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc);

   if (err) {
      if (log_cb) {
         log_cb(RETRO_LOG_ERROR, "Failed on uc_open with error returned: %u\n", err);
      }
   }
}

RETRO_API void retro_deinit(void)
{
   if (frame_buf) {
      delete[] frame_buf;
      frame_buf = nullptr;
   }
   
   if (uc) {
      uc_close(uc);
      uc = nullptr;
   }
}

RETRO_API unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   if (log_cb) {
      log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
   }
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Libretro Unicorn Core";
   info->library_version  = "0.1";
   info->need_fullpath    = true;
   info->valid_extensions = "bin|rom";
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   float aspect                = (float)screenWidth / (float)screenHeight;
   float sampling_rate         = audioSampleRate * 1.0f;

   info->geometry.base_width   = screenWidth;
   info->geometry.base_height  = screenHeight;
   info->geometry.max_width    = screenWidth;
   info->geometry.max_height   = screenHeight;
   info->geometry.aspect_ratio = aspect;
   info->timing.fps            = fps;
   info->timing.sample_rate    = sampling_rate;

   last_aspect                 = aspect;
   last_sample_rate            = sampling_rate;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   else
      log_cb = fallback_log;

   static const struct retro_controller_description controllers[] = {
      { "RetroPad", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) },
   };

   static const struct retro_controller_info ports[] = {
      { controllers, 1 },
      { NULL, 0 },
   };

   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }

RETRO_API void retro_reset(void) {}

static void update_input(void)
{
   // Example of reading standard RetroPad input
   // bool start_pressed = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
   // bool a_pressed = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
}

static void check_variables(void) {}

// Called by frontend on a background thread IF use_audio_cb is true
static void audio_callback(void)
{
   int16_t audio_buf[samplesPerFrame * 2];
   generate_audio_samples(audio_buf, samplesPerFrame);
   audio_batch_cb(audio_buf, samplesPerFrame);
}

static void audio_set_state(bool enable) { (void)enable; }

static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t mod) {}

RETRO_API void retro_run(void)
{
   // 1. MUST POLL INPUT EVERY FRAME
   if (input_poll_cb) {
      input_poll_cb();
   }
   
   // 2. Process Input
   update_input();

   // 3. Emulate a frame here (Unicorn step)
   // uc_emu_start(uc, ...);

   // 4. Output Video
   video_cb(frame_buf, screenWidth, screenHeight, screenWidth * sizeof(uint32_t));

   // 5. Output Audio (Fallback for frontends that don't support threaded audio_callback)
   if (!use_audio_cb) {
      int16_t audio_buf[samplesPerFrame * 2];
      generate_audio_samples(audio_buf, samplesPerFrame);
      audio_batch_cb(audio_buf, samplesPerFrame);
   }

   // 6. Check environment variables
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
      check_variables();
   }
}

RETRO_API bool retro_load_game(const struct retro_game_info *info)
{
   if (!info || !info->path) {
       // Frontend didn't provide a path (e.g. subsystem / memory loading)
       retro_game_path[0] = '\0';
   } else {
       snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);
   }

   // Standard RetroPad descriptor
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A Button" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B Button" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb) log_cb(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
      return false;
   }

   struct retro_audio_callback audio_env_cb = { audio_callback, audio_set_state };
   use_audio_cb = environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio_env_cb);

   struct retro_keyboard_callback kb_cb = { keyboard_cb };
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_cb);

   check_variables();

   return true;
}

RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) { return false; }
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data_, size_t size) { return false; }
RETRO_API bool retro_unserialize(const void *data_, size_t size) { return false; }
RETRO_API void *retro_get_memory_data(unsigned id) { return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return 0; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) {}

} // extern "C"