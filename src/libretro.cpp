/**
 * Libretro C++ Core: Spheroid (Unicorn Engine - ARM64)
 * -----------------------------------------------------------------------------
 * Software Rendering enabled (640x480).
 * Supported inputs: RetroPad w/ Analog, Mouse, Keyboard.
 * Requires Content: Booting without a ROM is disabled. Full paths enforced.
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#define _USE_MATH_DEFINES
#include <math.h>
#include <elf.h>
#include <gpu.hpp>
#include <HandmadeMath.h>
#include <vector>

#if defined(_WIN32) && !defined(_XBOX)
#include <windows.h>
#endif

#include "libretro.h"
#include <unicorn/unicorn.h>

extern "C" {

// =============================================================================
// Core Configuration & Globals
// =============================================================================

/* Spheroid Fantasy Console Target Resolution */
constexpr unsigned SCREEN_WIDTH  = 320;
constexpr unsigned SCREEN_HEIGHT = 240;
constexpr unsigned TOTAL_PIXELS  = SCREEN_WIDTH * SCREEN_HEIGHT;
constexpr float ASPECT_RATIO = 4.f / 3.0f;

/* Audio Configuration */
constexpr unsigned AUDIO_SAMPLE_RATE = 44100;
constexpr unsigned FPS = 60;
constexpr unsigned SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE / FPS; // 735 samples
constexpr auto cpuMips = 50;

// Unicorn requires memory sizes and addresses to be multiples of 4KB (4096 bytes).
// 128 MB is perfectly aligned (134,217,728 bytes).
const uint32_t RAM_SIZE = 128 * 1024 * 1024;

// Where does the RAM start in the emulated CPU's address space?
// We use 0x10000000 to avoid Address 0 (which catches NULL pointer bugs in games).
const uint64_t RAM_BASE_ADDRESS = 0x10000000;

// The actual host memory block
static uint8_t* console_ram = nullptr;

static struct retro_log_callback logging;
static retro_log_printf_t log_cb;

char system_directory[4096]; 
char save_directory[4096];   
char retro_game_path[4096];  

// --- Software Rendering Buffer ---
// 32-bit XRGB8888 buffer where your TBDR will draw its pixels.
static uint32_t *frame_buf = nullptr;

// --- Unicorn Engine Variables ---
static uc_engine *uc = nullptr;
static uc_context *uc_ctx = nullptr; // Used for Savestates

// --- Save RAM & Save States ---
static uint8_t sram[0x2000]; // Dummy 8KB Save RAM
struct CoreState {
   uint32_t frame_count;
   double internal_timer;
   // Add custom fantasy console hardware registers here (GPU regs, Audio regs, etc.)
};
static CoreState core_state = {0, 0.0};

static SpheroidGPU gpu;

struct Mesh {
    std::vector<HMM_Vec3> vertices;
    std::vector<HMM_Vec3> colors; // Using Vec3 for RGB 0-255 is fine here
    std::vector<int> indices;
};

static float rotation_3d = 0.0f;
static Mesh demo_cube = []() {
    Mesh m;
    m.vertices = {
        {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
        {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1}
    };
    m.colors = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
        {255, 0, 255}, {0, 255, 255}, {255, 255, 255}, {128, 128, 128}
    };
    m.indices = {
        0,1,2, 0,2,3,  1,5,6, 1,6,2,  5,4,7, 5,7,6,
        4,0,3, 4,3,7,  3,2,6, 3,6,7,  4,5,1, 4,1,0 
    };
    return m;
}();

constexpr uint32_t VRAM_LIST_OFFSET   = 0x01000000; 
constexpr uint32_t VRAM_VERTEX_OFFSET = 0x02000000; 
constexpr uint32_t VRAM_TEXTURE_OFFSET = 0x03000000;

// Change this number to stress the CPU! 
// 512 cubes = 18,432 triangles per frame.
constexpr int GRID_SIZE = 2; 
constexpr int TOTAL_CUBES = GRID_SIZE * GRID_SIZE * GRID_SIZE; 

static void simulate_guest_game() {
    rotation_3d += 0.02f; 
    float rot = rotation_3d;
    
    static bool uploaded = false;
    if (!uploaded) {
        // 1. Generate a 64x64 Checkerboard Texture
        uint32_t* tex_ram = (uint32_t*)&console_ram[VRAM_TEXTURE_OFFSET];
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                bool is_white = ((x / 8) % 2) == ((y / 8) % 2);
                tex_ram[y * 64 + x] = is_white ? 0xFFFFFFFF : 0xFF444444; // White/Dark Gray
            }
        }

        // 2. Upload Mesh with UVs
        // A single 8-vertex cube shares vertices, so UVs wrap weirdly, 
        // but it's enough to prove the texture mapper works!
        GPUVertex* vram_verts = (GPUVertex*)&console_ram[VRAM_VERTEX_OFFSET];
        int v_count = 0;
        
        // Quick & Dirty Procedural UV mapping for the demo
        HMM_Vec2 fake_uvs[3] = { {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f} };

        for (size_t j = 0; j < demo_cube.indices.size(); j++) {
            int idx = demo_cube.indices[j];
            vram_verts[v_count++] = {
                demo_cube.vertices[idx].X, demo_cube.vertices[idx].Y, demo_cube.vertices[idx].Z,
                fake_uvs[j % 3].X, fake_uvs[j % 3].Y, // UVs
                (uint8_t)demo_cube.colors[idx].X, (uint8_t)demo_cube.colors[idx].Y, (uint8_t)demo_cube.colors[idx].Z, 255
            };
        }
        uploaded = true;
    }

    uint32_t* cmd_list = (uint32_t*)&console_ram[VRAM_LIST_OFFSET];
    uint32_t c = 0; 
    
    cmd_list[c++] = 0x01; // CLEAR
    cmd_list[c++] = 0xFF101020; 

    // NEW: Bind the texture!
    cmd_list[c++] = 0x03; // BIND_TEXTURE
    cmd_list[c++] = VRAM_TEXTURE_OFFSET;
    cmd_list[c++] = 64;   // Width
    cmd_list[c++] = 64;   // Height

    HMM_Mat4 proj = HMM_Perspective_LH_ZO(1.2f, ASPECT_RATIO, 0.1f, 100.0f);
    HMM_Mat4 rotX = HMM_Rotate_LH(rot * 0.2f, HMM_V3(1.0f, 0.0f, 0.0f));
    HMM_Mat4 rotY = HMM_Rotate_LH(rot * 0.3f, HMM_V3(0.0f, 1.0f, 0.0f));
    HMM_Mat4 view = HMM_Translate(HMM_V3(0.0f, 0.0f, 20.0f)) * (rotY * rotX);
    HMM_Mat4 view_proj = proj * view;

    for (int i = 0; i < TOTAL_CUBES; i++) {
        float x = (i % GRID_SIZE) - (GRID_SIZE / 2.0f);
        float y = ((i / GRID_SIZE) % GRID_SIZE) - (GRID_SIZE / 2.0f);
        float z = ((i / (GRID_SIZE * GRID_SIZE)) % GRID_SIZE) - (GRID_SIZE / 2.0f);

        float local_rot = rot * 2.0f + (x * 0.5f);

        HMM_Mat4 m_trans = HMM_Translate(HMM_V3(x * 2.5f, y * 2.5f, z * 2.5f));
        HMM_Mat4 m_rotX  = HMM_Rotate_LH(local_rot, HMM_V3(1.0f, 0.0f, 0.0f));
        HMM_Mat4 m_rotY  = HMM_Rotate_LH(local_rot, HMM_V3(0.0f, 1.0f, 0.0f));
        HMM_Mat4 m_scale = HMM_Scale(HMM_V3(0.4f, 0.4f, 0.4f));
        
        HMM_Mat4 mvp = view_proj * m_trans * (m_rotY * m_rotX) * m_scale;

        cmd_list[c++] = 0x02; // SET_MATRIX
        memcpy(&cmd_list[c], &mvp, sizeof(HMM_Mat4));
        c += 16; 

        cmd_list[c++] = 0x04; // DRAW_ARRAYS
        cmd_list[c++] = VRAM_VERTEX_OFFSET;
        cmd_list[c++] = 36; 
    }
    
    cmd_list[c++] = 0xFF; // END LIST

    gpu.execute_display_list(VRAM_LIST_OFFSET);
}
// =============================================================================
// Libretro Callbacks
// =============================================================================
static retro_environment_t environ_cb;           
static retro_video_refresh_t video_cb;           
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;         
static retro_input_state_t input_state_cb;       
static retro_audio_sample_t audio_cb; // Legacy single-sample fallback

// =============================================================================
// Utilities
// =============================================================================
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

static void show_message(const char* msg, unsigned frames)
{
   if (!environ_cb) return;
   struct retro_message retro_msg = { msg, frames };
   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &retro_msg);
}

static void generate_audio(int16_t *buffer, size_t num_frames)
{
   static double phase = 0.0;
   const double phase_increment = (2.0 * M_PI * 440.0) / AUDIO_SAMPLE_RATE;

   for (size_t i = 0; i < num_frames; i++) {
      int16_t sample = (int16_t)(0x800 * sin(phase));
      buffer[i * 2 + 0] = sample; 
      buffer[i * 2 + 1] = sample; 
      phase += phase_increment;
      if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
   }
}

// =============================================================================
// Libretro Core Lifecycle
// =============================================================================
RETRO_API void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
   else log_cb = fallback_log;

   // ENFORCE CONTENT: Spheroid requires a ROM to be loaded.
   bool no_rom = false;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

   // Declare supported inputs: RetroPad (+Analog), Mouse, Keyboard.
   static const struct retro_controller_description controllers[] = {
      { "RetroPad", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) },
      { "RetroPad w/ Analog", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0) },
      { "Mouse", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0) },
      { "Keyboard", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_KEYBOARD, 0) },
   };

   static const struct retro_controller_info ports[] = {
      { controllers, 4 }, { nullptr, 0 },
   };
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

   // Core Options
   struct retro_variable variables[] = {
      { "spheroid_tbdr_threads", "Renderer Threads; 1|2|4|8" },
      { nullptr, nullptr },
   };
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

RETRO_API void retro_init(void)
{
   // Allocate the software framebuffer
   if (!frame_buf) {
      frame_buf = new uint32_t[TOTAL_PIXELS];
   }
   memset(frame_buf, 0, TOTAL_PIXELS * sizeof(uint32_t));

   const char *sys_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir) {
      snprintf(system_directory, sizeof(system_directory), "%s", sys_dir);
   }

   const char *sav_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sav_dir) && sav_dir) {
      snprintf(save_directory, sizeof(save_directory), "%s", sav_dir);
   }

   memset(sram, 0, sizeof(sram));

   // Initialize Unicorn Engine for ARM64
   uc_err err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc);
   if (err && log_cb) {
      log_cb(RETRO_LOG_ERROR, "Failed to init Unicorn Engine: %u (%s)\n", err, uc_strerror(err));
   } else if (uc) {
      // Allocate context memory used for Save States
      uc_context_alloc(uc, &uc_ctx);
   }
   
   // 1. Allocate 128MB of memory and zero it out
    console_ram = (uint8_t*)calloc(1, RAM_SIZE);
    if (!console_ram) {
        printf("Failed to allocate 128MB of host RAM!\n");
        return; 
    }

    // 2. Map this exact pointer directly into the Unicorn Engine
    // UC_PROT_ALL means the emulated CPU can Read, Write, and Execute code here.
    err = uc_mem_map_ptr(uc, RAM_BASE_ADDRESS, RAM_SIZE, UC_PROT_ALL, console_ram);
    
    if (err != UC_ERR_OK) {
        printf("Unicorn Failed to map memory: %s\n", uc_strerror(err));
        free(console_ram);
        console_ram = nullptr;	
        return;
    }
	
	gpu.init(SCREEN_WIDTH, SCREEN_HEIGHT);
	gpu.set_ram_pointer(console_ram);
}

RETRO_API void retro_deinit(void)
{
   if (frame_buf) {
      delete[] frame_buf;
      frame_buf = nullptr;
   }
   if (uc_ctx) {
      uc_free(uc_ctx);
      uc_ctx = nullptr;
   }
   if (uc) {
      uc_close(uc);
      uc = nullptr;
   }
   if (console_ram) {
        // Unicorn will unmap automatically when uc_close() is called,
        // but we still need to free our host memory.
        free(console_ram);
        console_ram = nullptr;
    }
	 gpu.shutdown();
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Spheroid";
   info->library_version  = "0.1";
   info->need_fullpath    = true; // Enforce full absolute file paths
   info->valid_extensions = "bin|rom|sph"; 
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width   = SCREEN_WIDTH;
   info->geometry.base_height  = SCREEN_HEIGHT;
   info->geometry.max_width    = SCREEN_WIDTH;
   info->geometry.max_height   = SCREEN_HEIGHT;
   info->geometry.aspect_ratio = ASPECT_RATIO;
   
   info->timing.fps            = FPS;
   info->timing.sample_rate    = AUDIO_SAMPLE_RATE;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }

RETRO_API void retro_reset(void)
{
   core_state.frame_count = 0;
   core_state.internal_timer = 0.0;
   
   // Reset Unicorn CPU registers to boot state
   if (uc) {
      // uint64_t boot_addr = 0x10000;
      // uc_reg_write(uc, UC_ARM64_REG_PC, &boot_addr);
      
      // uint64_t sp_addr = 0x200000;
      // uc_reg_write(uc, UC_ARM64_REG_SP, &sp_addr);
   }
}

static void check_variables(void)
{
   struct retro_variable var = {0};
   var.key = "spheroid_tbdr_threads";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      // Handle option updates here
   }
}

// Hardware Keyboard hook
static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t mod) {
   // Map raw keyboard events to your emulated hardware here
}

RETRO_API void retro_run(void)
{
   // 1. Mandatory Input Poll
   if (input_poll_cb) input_poll_cb();
   
   // Read Inputs
   // bool btn_start = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
   // int16_t mouse_x = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
   
   // 2. Emulate Frame (Unicorn Step)
   core_state.frame_count++;
   
   
   uint64_t current_pc;
    uc_reg_read(uc, UC_ARM64_REG_PC, &current_pc);
	log_cb(RETRO_LOG_ERROR,"%X\n", current_pc);
    // Run the ARM64 emulator!
    // Parameters: engine, start_address, until_address, timeout (0=infinite), count (0=infinite)
    // For a game console, you usually run for a set number of instructions or until a specific "Yield" address.
    // For testing, we just run 2 instructions.
	constexpr auto cpuIpf = cpuMips * 1000000 / FPS;
    uc_err err = uc_emu_start(uc, current_pc, 0xFFFFFFFFFFFFFFFF , 0, cpuIpf);

    if (err) {
        // Write error to a file
        FILE* f = fopen("unicorn_debug.txt", "w");
        if (f) {
            fprintf(f, "CPU Crash: %s\n", uc_strerror(err));
            fclose(f);
        }
    }

   simulate_guest_game();
   
   // Push the completed software frame to the frontend.
   // Pitch = the width of the screen multiplied by the size of a single pixel (4 bytes for XRGB8888).
   video_cb(gpu.get_framebuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint32_t));

   // 4. Audio Output
   int16_t audio_buf[SAMPLES_PER_FRAME * 2];
   generate_audio(audio_buf, SAMPLES_PER_FRAME); 
   audio_batch_cb(audio_buf, SAMPLES_PER_FRAME);

   // 5. Options Update
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
      check_variables();
   }
}

RETRO_API bool retro_load_game(const struct retro_game_info *info)
{
   // Strict constraint: Must have a valid ROM path!
   if (!info || !info->path) {
      if (log_cb) log_cb(RETRO_LOG_ERROR, "Spheroid requires a valid ROM path to boot.\n");
      return false;
   }

   snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);

   // 1. Pixel Format (32-bit color: 0x00RRGGBB)
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
      if (log_cb) log_cb(RETRO_LOG_ERROR, "XRGB8888 is not supported by frontend.\n");
      return false;
   }

   // 2. Setup Exhaustive Input Descriptors
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A (Right)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B (Down)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X (Up)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y (Left)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L1 / Left Bumper" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R1 / Right Bumper" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "L2 / Left Trigger" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "R2 / Right Trigger" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "L3 / Left Stick Click" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "R3 / Right Stick Click" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },
      { 0 }, 
   };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
	
   // 3. Load Content into Unicorn
   /*
   FILE *rom = fopen(retro_game_path, "rb");
   if (rom) {
       fseek(rom, 0, SEEK_END);
       size_t rom_size = ftell(rom);
       fseek(rom, 0, SEEK_SET);
       uint8_t *rom_data = new uint8_t[rom_size];
       fread(rom_data, 1, rom_size, rom);
       fclose(rom);

       // Map memory and write ROM
       uc_mem_map(uc, 0x10000, 2 * 1024 * 1024, UC_PROT_ALL);
       uc_mem_write(uc, 0x10000, rom_data, rom_size);
       delete[] rom_data;
   }
   */

	// 1. Put some dummy ARM64 machine code into our RAM (at the beginning).
    // In a real scenario, you would use fread() to load a game's ".bin" file into console_ram here.
    
    // This is ARM64 machine code for: 
    // mov w0, #0x1234
    // b .-4  
    uint32_t boot_code[] = { 0x52a24680, 0x17ffffff   }; 
    memcpy(&console_ram[0], boot_code, sizeof(boot_code));

    // 2. Set the Stack Pointer (SP) to the very top of our 128MB RAM.
    uint64_t sp_value = RAM_BASE_ADDRESS + RAM_SIZE;
    uc_reg_write(uc, UC_ARM64_REG_SP, &sp_value);

    // 3. Set the Program Counter (PC) to the start of our RAM (where our boot code is).
    uint64_t pc_value = RAM_BASE_ADDRESS;
    uc_reg_write(uc, UC_ARM64_REG_PC, &pc_value);

   // 4. Connect Keyboard Hook
   struct retro_keyboard_callback kb_cb = { keyboard_cb };
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_cb);

   check_variables();
   retro_reset();

   return true;
}

RETRO_API void retro_unload_game(void) {

}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

// =============================================================================
// Serialization (Savestates & Rewind)
// =============================================================================

RETRO_API size_t retro_serialize_size(void) { 
   size_t context_size = 0;
   if (uc && uc_ctx) {
      context_size = uc_context_size(uc);
   }
   return sizeof(CoreState) + sizeof(sram) + context_size; 
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
   if (size < retro_serialize_size()) return false;
   uint8_t *ptr = (uint8_t*)data;
   
   // 1. Save Core State
   memcpy(ptr, &core_state, sizeof(CoreState)); 
   ptr += sizeof(CoreState);
   
   // 2. Save SRAM
   memcpy(ptr, sram, sizeof(sram));
   ptr += sizeof(sram);

   // 3. Save Unicorn CPU Context
   if (uc && uc_ctx) {
      uc_context_save(uc, uc_ctx);
      size_t context_size = uc_context_size(uc);
      memcpy(ptr, uc_ctx, context_size);
   }
   
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   if (size < retro_serialize_size()) return false;
   const uint8_t *ptr = (const uint8_t*)data;
   
   // 1. Restore Core State
   memcpy(&core_state, ptr, sizeof(CoreState)); 
   ptr += sizeof(CoreState);
   
   // 2. Restore SRAM
   memcpy(sram, ptr, sizeof(sram));
   ptr += sizeof(sram);

   // 3. Restore Unicorn CPU Context
   if (uc && uc_ctx) {
      size_t context_size = uc_context_size(uc);
      memcpy(uc_ctx, ptr, context_size);
      uc_context_restore(uc, uc_ctx);
   }
   
   return true;
}

// =============================================================================
// Save Data (SRAM / Memory Cards)
// =============================================================================
RETRO_API void *retro_get_memory_data(unsigned id) { 
   return (id == RETRO_MEMORY_SAVE_RAM) ? sram : nullptr; 
}
RETRO_API size_t retro_get_memory_size(unsigned id) { 
   return (id == RETRO_MEMORY_SAVE_RAM) ? sizeof(sram) : 0; 
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

} // extern "C"