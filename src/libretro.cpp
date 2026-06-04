/**
 * Libretro C++ Core: Spheroid (Unicorn Engine - ARM64)
 * -----------------------------------------------------------------------------
 * Target Resolution: 320x240 (TBDR Software Renderer)
 * Architecture: ARM64 Guest via Unicorn Engine
 * Interfacing: Syscall (SVC) based hardware abstraction
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#include "elf.h"
#include "gpu.hpp"
#include "HandmadeMath.h"
#include "libretro.h"
#include <unicorn/unicorn.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern "C" {

// =============================================================================
// Core Configuration & Globals
// =============================================================================

constexpr unsigned SCREEN_WIDTH  = 320;
constexpr unsigned SCREEN_HEIGHT = 240;
constexpr float ASPECT_RATIO     = 4.f / 3.0f;

constexpr unsigned AUDIO_SAMPLE_RATE = 44100;
constexpr unsigned FPS = 60;
constexpr unsigned SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE / FPS; // 735 samples
constexpr auto cpuMips = 50;

// RAM Configuration (128 MB)
const uint32_t RAM_SIZE = 128 * 1024 * 1024;
const uint64_t RAM_BASE_ADDRESS = 0x10000000;
static uint8_t* console_ram = nullptr;

constexpr uint32_t VRAM_LIST_OFFSET   = 0x01000000; 
constexpr uint32_t VRAM_VERTEX_OFFSET = 0x02000000; 
constexpr uint32_t VRAM_TEXTURE_OFFSET = 0x03000000;

// Libretro / Frontend Globals
static struct retro_log_callback logging;
static retro_log_printf_t log_cb;
char system_directory[4096]; 
char save_directory[4096];   
char retro_game_path[4096];  

// Emulator Subsystems
static SpheroidGPU gpu;
static uc_engine *uc = nullptr;
static uc_context *uc_ctx = nullptr; 
static uc_hook syscall_hook;

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
// SYSCALL HANDLER (Replaces MMIO)
// =============================================================================
// ABI Convention:
// X8 = Syscall Number
// X0, X1, X2 = Arguments
// =============================================================================
static void syscall_interrupt_cb(uc_engine *uc, uint32_t intno, void *user_data) {
    // In Unicorn ARM64, the intno usually maps to the exception type. 
    // We just need to read the X8 register to see what the guest wants.
    uint64_t syscall_num = 0;
    uc_reg_read(uc, UC_ARM64_REG_X8, &syscall_num);

    switch (syscall_num) {
        case 1: { // SYSCALL 1: DEBUG_PRINT
            uint64_t char_val = 0;
            uc_reg_read(uc, UC_ARM64_REG_X0, &char_val);
            
            // Buffer characters until a newline is hit, then send to Libretro log
            static std::string log_buffer;
            char c = (char)(char_val & 0xFF);
            if (c == '\n') {
                if (log_cb) log_cb(RETRO_LOG_INFO, "[GUEST] %s\n", log_buffer.c_str());
                log_buffer.clear();
            } else {
                log_buffer += c;
            }
            break;
        }
        case 2: { // SYSCALL 2: YIELD (End of Frame)
            // Immediately pause CPU execution and return control to Libretro frontend.
            // Unicorn automatically leaves the PC pointing at the NEXT instruction!
            uc_emu_stop(uc);
            break;
        }
        default: {
            if (log_cb) log_cb(RETRO_LOG_WARN, "[SYSCALL] Unknown Syscall: %lu\n", syscall_num);
            break;
        }
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

   // Initialize Unicorn Engine
   uc_err err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc);
   if (err && log_cb) {
      log_cb(RETRO_LOG_ERROR, "Failed to init Unicorn Engine: %u (%s)\n", err, uc_strerror(err));
   } else if (uc) {
      uc_context_alloc(uc, &uc_ctx);
   }
   
   // Allocate and Map 128MB RAM
   console_ram = (uint8_t*)calloc(1, RAM_SIZE);
   if (console_ram) {
       err = uc_mem_map_ptr(uc, RAM_BASE_ADDRESS, RAM_SIZE, UC_PROT_ALL, console_ram);
       if (err != UC_ERR_OK) {
           if (log_cb) log_cb(RETRO_LOG_ERROR, "Unicorn Failed to map memory: %s\n", uc_strerror(err));
       }
   }

   gpu.init(SCREEN_WIDTH, SCREEN_HEIGHT);
   gpu.set_ram_pointer(console_ram);
}

RETRO_API void retro_deinit(void) {
   gpu.shutdown();
   if (uc_ctx) { uc_free(uc_ctx); uc_ctx = nullptr; }
   if (uc) { uc_close(uc); uc = nullptr; }
   if (console_ram) { free(console_ram); console_ram = nullptr; }
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
   memset(info, 0, sizeof(*info));
   info->library_name     = "Spheroid";
   info->library_version  = "0.2";
   info->need_fullpath    = true; 
   info->valid_extensions = "bin|rom|sph"; 
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
   // In the future, re-read the ELF entry point and reset PC/SP here
}

RETRO_API void retro_run(void) {
   if (input_poll_cb) input_poll_cb();
   
   core_state.frame_count++;
   
   uint64_t current_pc;
   uc_reg_read(uc, UC_ARM64_REG_PC, &current_pc);

   // Run the CPU until a Syscall Yield (uc_emu_stop) or IPF timeout is reached.
   constexpr auto cpuIpf = cpuMips * 1000000 / FPS;
   uc_err err = uc_emu_start(uc, current_pc, 0xFFFFFFFFFFFFFFFF, 0, cpuIpf);

   if (err && log_cb) {
       log_cb(RETRO_LOG_ERROR, "[CPU CRASH] Err: %u (%s) at PC: 0x%lX\n", err, uc_strerror(err), current_pc);
   }

   gpu.execute_display_list(VRAM_LIST_OFFSET);
   video_cb(gpu.get_framebuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint32_t));

   int16_t audio_buf[SAMPLES_PER_FRAME * 2];
   generate_audio(audio_buf, SAMPLES_PER_FRAME); 
   audio_batch_cb(audio_buf, SAMPLES_PER_FRAME);
}

static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t mod) {
   // Hardware Keyboard hook
}

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
   if (!info || !info->path) return false;
   snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;

   // Setup Interrupt Hook for Syscalls
   uc_hook_add(uc, &syscall_hook, UC_HOOK_INTR, (void*)syscall_interrupt_cb, nullptr, 1, 0);

   // Load ELF File
   FILE *rom = fopen(retro_game_path, "rb");
   if (!rom) return false;
   fseek(rom, 0, SEEK_END);
   size_t rom_size = ftell(rom);
   fseek(rom, 0, SEEK_SET);

   std::vector<uint8_t> elf_data(rom_size);
   fread(elf_data.data(), 1, rom_size, rom);
   fclose(rom);

   if (rom_size < sizeof(Elf64_Ehdr)) return false;
   Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_data.data();

   if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return false;
   if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_machine != EM_AARCH64) return false;

   Elf64_Phdr* phdr = (Elf64_Phdr*)(elf_data.data() + ehdr->e_phoff);
   
   for (int i = 0; i < ehdr->e_phnum; i++) {
       if (phdr[i].p_type == PT_LOAD && phdr[i].p_memsz > 0) {
           uint64_t vaddr = phdr[i].p_vaddr;
           uint64_t memsz = phdr[i].p_memsz;
           uint64_t filesz = phdr[i].p_filesz;
           uint64_t offset = phdr[i].p_offset;

           if (vaddr >= RAM_BASE_ADDRESS && (vaddr + memsz) <= (RAM_BASE_ADDRESS + RAM_SIZE)) {
               uint32_t ram_offset = (uint32_t)(vaddr - RAM_BASE_ADDRESS);
               if (filesz > 0) memcpy(&console_ram[ram_offset], elf_data.data() + offset, filesz);
               if (memsz > filesz) memset(&console_ram[ram_offset + filesz], 0, memsz - filesz);
           }
       }
   }

   uint64_t sp_value = RAM_BASE_ADDRESS + RAM_SIZE;
   uc_reg_write(uc, UC_ARM64_REG_SP, &sp_value);

   uint64_t pc_value = ehdr->e_entry;
   uc_reg_write(uc, UC_ARM64_REG_PC, &pc_value);

   struct retro_keyboard_callback kb_cb = { keyboard_cb };
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_cb);

   retro_reset();
   return true;
}

RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

// =============================================================================
// Serialization (Savestates & Rewind)
// =============================================================================

RETRO_API size_t retro_serialize_size(void) { 
   size_t context_size = 0;
   if (uc && uc_ctx) context_size = uc_context_size(uc);
   // MUST include RAM_SIZE for savestates to work properly on an emulator!
   return sizeof(CoreState) + sizeof(sram) + RAM_SIZE + context_size; 
}

RETRO_API bool retro_serialize(void *data, size_t size) {
   if (size < retro_serialize_size()) return false;
   uint8_t *ptr = (uint8_t*)data;
   
   memcpy(ptr, &core_state, sizeof(CoreState)); ptr += sizeof(CoreState);
   memcpy(ptr, sram, sizeof(sram)); ptr += sizeof(sram);
   
   // Save 128MB System RAM
   memcpy(ptr, console_ram, RAM_SIZE); ptr += RAM_SIZE;

   if (uc && uc_ctx) {
      uc_context_save(uc, uc_ctx);
      size_t context_size = uc_context_size(uc);
      memcpy(ptr, uc_ctx, context_size);
   }
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
   if (size < retro_serialize_size()) return false;
   const uint8_t *ptr = (const uint8_t*)data;
   
   memcpy(&core_state, ptr, sizeof(CoreState)); ptr += sizeof(CoreState);
   memcpy(sram, ptr, sizeof(sram)); ptr += sizeof(sram);
   
   // Restore 128MB System RAM
   memcpy(console_ram, ptr, RAM_SIZE); ptr += RAM_SIZE;

   if (uc && uc_ctx) {
      size_t context_size = uc_context_size(uc);
      memcpy(uc_ctx, ptr, context_size);
      uc_context_restore(uc, uc_ctx);
   }
   return true;
}

RETRO_API void *retro_get_memory_data(unsigned id) { return (id == RETRO_MEMORY_SAVE_RAM) ? sram : nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return (id == RETRO_MEMORY_SAVE_RAM) ? sizeof(sram) : 0; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

} // extern "C"