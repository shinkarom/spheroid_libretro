#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// Include libretro header for the log callback type
#include "libretro.h"

extern "C" {
#include "quickjs.h"
}

class SpheroidScript {
public:
    SpheroidScript() = default;
    ~SpheroidScript() { shutdown(); }

    // Initialize the JS Runtime and map the C++ RAM to a JS ArrayBuffer
    bool init(uint8_t* ram_ptr, size_t ram_size, retro_log_printf_t log_cb);
    
    // Shut down runtime
    void shutdown();

    // Load and evaluate the JS ROM
    bool load_game(const char* js_source, size_t source_size);

    // Execution Hooks
    void call_init();
    void call_update();

private:
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    JSValue js_init_func = JS_UNDEFINED;
    JSValue js_update_func = JS_UNDEFINED;

    retro_log_printf_t logger = nullptr;

    // Helper to print JS errors (syntax errors, runtime exceptions) to Libretro
    void print_js_exception();

    // Static wrapper for JS to avoid C++ member function pointer issues
    static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
    
    // Dummy free function because C++ owns the RAM, not the JS Garbage Collector
    static void dummy_free_ram(JSRuntime *rt, void *opaque, void *ptr) {}
};