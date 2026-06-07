#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "libretro.h"
#include "vfs.hpp"

extern "C" {
#include "quickjs.h"
}

class SpheroidScript {
public:
    SpheroidScript() = default;
    ~SpheroidScript() { shutdown(); }

    // Initialize the JS Runtime, array buffers, and VFS bindings
    bool init(uint8_t* ram_ptr, size_t ram_size, VFSManager* vfs_mgr, retro_log_printf_t log_cb);
    
    void shutdown();

    // Compile and execute the main JS file as an ES6 Module
    bool load_game(const char* js_source, size_t source_size);

    void call_init();
    void call_update();

private:
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    JSValue js_init_func = JS_UNDEFINED;
    JSValue js_update_func = JS_UNDEFINED;

    retro_log_printf_t logger = nullptr;
    
    // Core Engine Pointers
    VFSManager* vfs = nullptr;
    uint8_t* system_ram = nullptr;
    size_t system_ram_size = 0;

    void print_js_exception();

    // QuickJS Native C Bindings
    static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
    static void dummy_free_ram(JSRuntime *rt, void *opaque, void *ptr) {}
    
    // ES6 Module File Loader Callback
    static JSModuleDef* module_loader(JSContext *ctx, const char *module_name, void *opaque);

    // Native JS FileSystem Bindings
    static JSValue js_fs_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
    static JSValue js_fs_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
    static JSValue js_fs_seek(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
    static JSValue js_fs_tell(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
    static JSValue js_fs_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
};