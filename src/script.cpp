#include "script.hpp"
#include <vector>
#include <cstring>

// =============================================================================
// QuickJS C-Bindings & Error Handling
// =============================================================================

JSValue SpheroidScript::js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->logger) return JS_UNDEFINED;

    std::string output;
    for (int i = 0; i < argc; i++) {
        if (i != 0) output += " ";
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            output += str;
            JS_FreeCString(ctx, str);
        }
    }
    
    script->logger(RETRO_LOG_INFO, "[JS] %s\n", output.c_str());
    return JS_UNDEFINED;
}

void SpheroidScript::print_js_exception() {
    if (!logger) return;

    JSValue exception_val = JS_GetException(ctx);
    bool is_error = JS_IsError(ctx, exception_val);
    
    const char *err_msg = JS_ToCString(ctx, exception_val);
    if (err_msg) {
        logger(RETRO_LOG_ERROR, "[JS EXCEPTION] %s\n", err_msg);
        JS_FreeCString(ctx, err_msg);
    }

    if (is_error) {
        JSValue stack_val = JS_GetPropertyStr(ctx, exception_val, "stack");
        if (!JS_IsUndefined(stack_val)) {
            const char *stack_str = JS_ToCString(ctx, stack_val);
            if (stack_str) {
                logger(RETRO_LOG_ERROR, "[JS STACK]\n%s\n", stack_str);
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack_val);
    }
    
    JS_FreeValue(ctx, exception_val);
}

// =============================================================================
// ES6 Module Loader (Now uses VFSManager)
// =============================================================================

JSModuleDef* SpheroidScript::module_loader(JSContext *ctx, const char *module_name, void *opaque) {
    SpheroidScript* script = static_cast<SpheroidScript*>(opaque);

    // Clean up the requested path (e.g. remove "./" if present)
    std::string req_path = module_name;
    if (req_path.substr(0, 2) == "./") {
        req_path = req_path.substr(2);
    }

    // Use VFSManager to open the imported file
    int fd = script->vfs->open(req_path.c_str(), "r");
    if (fd < 0) {
        if (script->logger) script->logger(RETRO_LOG_ERROR, "[MODULE] Could not find: %s\n", req_path.c_str());
        return nullptr;
    }
    
    script->vfs->seek(fd, 0, 2); // SEEK_END
    int64_t size = script->vfs->tell(fd);
    script->vfs->seek(fd, 0, 0); // SEEK_SET

    std::vector<char> buffer(size + 1, '\0');
    script->vfs->read(fd, buffer.data(), size);
    script->vfs->close(fd);

    // Compile the imported file as an ES6 Module
    JSValue func_val = JS_Eval(ctx, buffer.data(), size, module_name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    
    if (JS_IsException(func_val)) {
        script->print_js_exception();
        return nullptr;
    }

    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);
    
    return m;
}

// =============================================================================
// Native JS FileSystem API
// =============================================================================

JSValue SpheroidScript::js_fs_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->vfs) return JS_NewInt32(ctx, -1);

    const char* path = JS_ToCString(ctx, argv[0]);
    const char* mode = JS_ToCString(ctx, argv[1]);
    if (!path || !mode) return JS_NewInt32(ctx, -1);

    int fd = script->vfs->open(path, mode);

    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, mode);
    return JS_NewInt32(ctx, fd);
}

JSValue SpheroidScript::js_fs_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->vfs) return JS_NewInt32(ctx, 0);

    int32_t fd, ram_offset, size;
    JS_ToInt32(ctx, &fd, argv[0]);
    JS_ToInt32(ctx, &ram_offset, argv[1]);
    JS_ToInt32(ctx, &size, argv[2]);

    // Safety check: Prevent JS from writing outside the RAM bounds!
    if (ram_offset < 0 || size <= 0 || ((size_t)ram_offset + size) > script->system_ram_size) {
        if (script->logger) script->logger(RETRO_LOG_WARN, "[VFS] Read out of RAM bounds!\n");
        return JS_NewInt32(ctx, 0);
    }

    int64_t bytes_read = script->vfs->read(fd, script->system_ram + ram_offset, size);
    return JS_NewInt32(ctx, bytes_read > 0 ? (int32_t)bytes_read : 0);
}

JSValue SpheroidScript::js_fs_seek(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->vfs) return JS_NewInt32(ctx, -1);

    int32_t fd, offset, whence;
    JS_ToInt32(ctx, &fd, argv[0]);
    JS_ToInt32(ctx, &offset, argv[1]);
    JS_ToInt32(ctx, &whence, argv[2]);

    int64_t res = script->vfs->seek(fd, offset, whence);
    return JS_NewInt32(ctx, (int32_t)res);
}

JSValue SpheroidScript::js_fs_tell(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->vfs) return JS_NewInt32(ctx, -1);

    int32_t fd;
    JS_ToInt32(ctx, &fd, argv[0]);

    int64_t res = script->vfs->tell(fd);
    return JS_NewInt32(ctx, (int32_t)res);
}

JSValue SpheroidScript::js_fs_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->vfs) return JS_UNDEFINED;

    int32_t fd;
    JS_ToInt32(ctx, &fd, argv[0]);

    script->vfs->close(fd);
    return JS_UNDEFINED;
}

// =============================================================================
// Core Implementation
// =============================================================================

bool SpheroidScript::init(uint8_t* ram_ptr, size_t ram_size, VFSManager* vfs_mgr, SpheroidAPU* apu_ptr, retro_log_printf_t log_cb) {
    logger = log_cb;
    vfs = vfs_mgr;
	apu = apu_ptr;
    system_ram = ram_ptr;
    system_ram_size = ram_size;

    rt = JS_NewRuntime();
    if (!rt) return false;

    ctx = JS_NewContext(rt);
    if (!ctx) return false;

    JS_SetContextOpaque(ctx, this);

    // Register Module Loader
    JS_SetModuleLoaderFunc(rt, nullptr, module_loader, this);

    // Create System object
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue system_obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, system_obj, "print", JS_NewCFunction(ctx, js_print, "print", 1));

    // Create System.fs object
    JSValue fs_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs_obj, "open", JS_NewCFunction(ctx, js_fs_open, "open", 2));
    JS_SetPropertyStr(ctx, fs_obj, "read", JS_NewCFunction(ctx, js_fs_read, "read", 3));
    JS_SetPropertyStr(ctx, fs_obj, "seek", JS_NewCFunction(ctx, js_fs_seek, "seek", 3));
    JS_SetPropertyStr(ctx, fs_obj, "tell", JS_NewCFunction(ctx, js_fs_tell, "tell", 1));
    JS_SetPropertyStr(ctx, fs_obj, "close", JS_NewCFunction(ctx, js_fs_close, "close", 1));

    // System.fs constants
    JS_SetPropertyStr(ctx, fs_obj, "SEEK_SET", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, fs_obj, "SEEK_CUR", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, fs_obj, "SEEK_END", JS_NewInt32(ctx, 2));

    JS_SetPropertyStr(ctx, system_obj, "fs", fs_obj);
	
	JSValue input_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, input_obj, "pressed", JS_NewCFunction(ctx, js_input_pressed, "pressed", 2));
    JS_SetPropertyStr(ctx, input_obj, "getPadState", JS_NewCFunction(ctx, js_input_get_pad_state, "getPadState", 1));

    // Define standard RetroPad Buttons as BITMASKS (1 << index)
    JS_SetPropertyStr(ctx, input_obj, "B",      JS_NewInt32(ctx, 1 << 0));
    JS_SetPropertyStr(ctx, input_obj, "Y",      JS_NewInt32(ctx, 1 << 1));
    JS_SetPropertyStr(ctx, input_obj, "SELECT", JS_NewInt32(ctx, 1 << 2));
    JS_SetPropertyStr(ctx, input_obj, "START",  JS_NewInt32(ctx, 1 << 3));
    JS_SetPropertyStr(ctx, input_obj, "UP",     JS_NewInt32(ctx, 1 << 4));
    JS_SetPropertyStr(ctx, input_obj, "DOWN",   JS_NewInt32(ctx, 1 << 5));
    JS_SetPropertyStr(ctx, input_obj, "LEFT",   JS_NewInt32(ctx, 1 << 6));
    JS_SetPropertyStr(ctx, input_obj, "RIGHT",  JS_NewInt32(ctx, 1 << 7));
    JS_SetPropertyStr(ctx, input_obj, "A",      JS_NewInt32(ctx, 1 << 8));
    JS_SetPropertyStr(ctx, input_obj, "X",      JS_NewInt32(ctx, 1 << 9));
    JS_SetPropertyStr(ctx, input_obj, "L",      JS_NewInt32(ctx, 1 << 10));
    JS_SetPropertyStr(ctx, input_obj, "R",      JS_NewInt32(ctx, 1 << 11));
    JS_SetPropertyStr(ctx, input_obj, "L2",     JS_NewInt32(ctx, 1 << 12));
    JS_SetPropertyStr(ctx, input_obj, "R2",     JS_NewInt32(ctx, 1 << 13));
    JS_SetPropertyStr(ctx, input_obj, "L3",     JS_NewInt32(ctx, 1 << 14));
    JS_SetPropertyStr(ctx, input_obj, "R3",     JS_NewInt32(ctx, 1 << 15));

    JS_SetPropertyStr(ctx, system_obj, "input", input_obj);
	
	JSValue audio_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, audio_obj, "write", JS_NewCFunction(ctx, js_apu_write, "write", 3));
    JS_SetPropertyStr(ctx, audio_obj, "read", JS_NewCFunction(ctx, js_apu_read, "read", 2));
    JS_SetPropertyStr(ctx, audio_obj, "writeGlobal", JS_NewCFunction(ctx, js_apu_write_global, "writeGlobal", 2));
    JS_SetPropertyStr(ctx, audio_obj, "readGlobal", JS_NewCFunction(ctx, js_apu_read_global, "readGlobal", 1));

    // Expose APU Constants
    JS_SetPropertyStr(ctx, audio_obj, "REG_START_ADDR",   JS_NewInt32(ctx, SpheroidAPU::REG_START_ADDR));
    JS_SetPropertyStr(ctx, audio_obj, "REG_END_ADDR",     JS_NewInt32(ctx, SpheroidAPU::REG_END_ADDR));
    JS_SetPropertyStr(ctx, audio_obj, "REG_LOOP_ADDR",    JS_NewInt32(ctx, SpheroidAPU::REG_LOOP_ADDR));
    JS_SetPropertyStr(ctx, audio_obj, "REG_LOOP_ENABLE",  JS_NewInt32(ctx, SpheroidAPU::REG_LOOP_ENABLE));
    JS_SetPropertyStr(ctx, audio_obj, "REG_PITCH",        JS_NewInt32(ctx, SpheroidAPU::REG_PITCH));
    JS_SetPropertyStr(ctx, audio_obj, "REG_VOL_LEFT",     JS_NewInt32(ctx, SpheroidAPU::REG_VOL_LEFT));
    JS_SetPropertyStr(ctx, audio_obj, "REG_VOL_RIGHT",    JS_NewInt32(ctx, SpheroidAPU::REG_VOL_RIGHT));
    JS_SetPropertyStr(ctx, audio_obj, "REG_ADSR_ATTACK",  JS_NewInt32(ctx, SpheroidAPU::REG_ADSR_ATTACK));
    JS_SetPropertyStr(ctx, audio_obj, "REG_ADSR_DECAY",   JS_NewInt32(ctx, SpheroidAPU::REG_ADSR_DECAY));
    JS_SetPropertyStr(ctx, audio_obj, "REG_ADSR_SUSTAIN", JS_NewInt32(ctx, SpheroidAPU::REG_ADSR_SUSTAIN));
    JS_SetPropertyStr(ctx, audio_obj, "REG_ADSR_RELEASE", JS_NewInt32(ctx, SpheroidAPU::REG_ADSR_RELEASE));
    JS_SetPropertyStr(ctx, audio_obj, "REG_DELAY_SEND",   JS_NewInt32(ctx, SpheroidAPU::REG_DELAY_SEND));
    JS_SetPropertyStr(ctx, audio_obj, "REG_CHANNELS",     JS_NewInt32(ctx, SpheroidAPU::REG_CHANNELS));
    JS_SetPropertyStr(ctx, audio_obj, "REG_PLAY_POS",     JS_NewInt32(ctx, SpheroidAPU::REG_PLAY_POS));
    JS_SetPropertyStr(ctx, audio_obj, "REG_ENV_LEVEL",    JS_NewInt32(ctx, SpheroidAPU::REG_ENV_LEVEL));

    JS_SetPropertyStr(ctx, audio_obj, "REG_GLOBAL_KEYON",       JS_NewInt32(ctx, SpheroidAPU::REG_GLOBAL_KEYON));
    JS_SetPropertyStr(ctx, audio_obj, "REG_GLOBAL_KEYOFF",      JS_NewInt32(ctx, SpheroidAPU::REG_GLOBAL_KEYOFF));
    JS_SetPropertyStr(ctx, audio_obj, "REG_GLOBAL_DELAY_LEN",   JS_NewInt32(ctx, SpheroidAPU::REG_GLOBAL_DELAY_LEN));
    JS_SetPropertyStr(ctx, audio_obj, "REG_GLOBAL_DELAY_FB",    JS_NewInt32(ctx, SpheroidAPU::REG_GLOBAL_DELAY_FB));
    JS_SetPropertyStr(ctx, audio_obj, "REG_GLOBAL_DELAY_VOL_L", JS_NewInt32(ctx, SpheroidAPU::REG_GLOBAL_DELAY_VOL_L));
    JS_SetPropertyStr(ctx, audio_obj, "REG_GLOBAL_DELAY_VOL_R", JS_NewInt32(ctx, SpheroidAPU::REG_GLOBAL_DELAY_VOL_R));
    JS_SetPropertyStr(ctx, audio_obj, "REG_GLOBAL_STATUS",      JS_NewInt32(ctx, SpheroidAPU::REG_GLOBAL_STATUS));

    JS_SetPropertyStr(ctx, system_obj, "audio", audio_obj);
	
    // Expose RAM ArrayBuffer
    JSValue ram_buffer = JS_NewArrayBuffer(ctx, ram_ptr, ram_size, dummy_free_ram, nullptr, false);
    JS_SetPropertyStr(ctx, system_obj, "RAM", ram_buffer);

    JS_SetPropertyStr(ctx, global_obj, "System", system_obj);
    JS_FreeValue(ctx, global_obj);

    return true;
}

void SpheroidScript::shutdown() {
    if (ctx) {
        JS_FreeValue(ctx, js_init_func);
        JS_FreeValue(ctx, js_update_func);
        JS_FreeContext(ctx); 
        ctx = nullptr; 
    }
    if (rt) { 
        JS_FreeRuntime(rt); 
        rt = nullptr; 
    }
    
    js_init_func = JS_UNDEFINED;
    js_update_func = JS_UNDEFINED;
}

bool SpheroidScript::load_game(const char* js_source, size_t source_size) {
    // Compile main script as an ES6 MODULE
    JSValue compile_res = JS_Eval(ctx, js_source, source_size, "boot.js", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    
    if (JS_IsException(compile_res)) {
        print_js_exception();
        return false;
    }

    JSModuleDef *module_def = (JSModuleDef *)JS_VALUE_GET_PTR(compile_res);

    // Execute the module (Recursively evaluates all imports)
    JSValue eval_res = JS_EvalFunction(ctx, compile_res);
    if (JS_IsException(eval_res)) {
        print_js_exception();
        JS_FreeValue(ctx, eval_res);
        return false;
    }
    JS_FreeValue(ctx, eval_res);

    // Look up the EXPORTED functions
    JSValue module_namespace = JS_GetModuleNamespace(ctx, module_def);
    js_init_func = JS_GetPropertyStr(ctx, module_namespace, "init");
    js_update_func = JS_GetPropertyStr(ctx, module_namespace, "update");
    JS_FreeValue(ctx, module_namespace);

    if (!JS_IsFunction(ctx, js_update_func)) {
        if (logger) logger(RETRO_LOG_ERROR, "Main JS file must 'export function update()'!\n");
        return false;
    }

    return true;
}

void SpheroidScript::call_init() {
    if (JS_IsFunction(ctx, js_init_func)) {
        JSValue result = JS_Call(ctx, js_init_func, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(result)) print_js_exception();
        JS_FreeValue(ctx, result);
    }
}

void SpheroidScript::call_update() {
    if (JS_IsFunction(ctx, js_update_func)) {
        JSValue result = JS_Call(ctx, js_update_func, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(result)) print_js_exception();
        JS_FreeValue(ctx, result);
    }

    JSContext *pctx;
    int err;
    for (;;) {
        err = JS_ExecutePendingJob(rt, &pctx);
        if (err <= 0) {
            if (err < 0) print_js_exception();
            break;
        }
    }
}

// =============================================================================
// Input API
// =============================================================================
void SpheroidScript::update_inputs(const uint16_t* pad_states) {
    for (int i = 0; i < 4; i++) {
        current_pad_state[i] = pad_states[i];
    }
}

JSValue SpheroidScript::js_input_pressed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script) return JS_FALSE;

    int32_t port, button_mask;
    JS_ToInt32(ctx, &port, argv[0]);
    JS_ToInt32(ctx, &button_mask, argv[1]);

    if (port < 0 || port >= 4) return JS_FALSE;

    // We now use the mask directly!
    bool is_pressed = (script->current_pad_state[port] & button_mask) == button_mask;
    return JS_NewBool(ctx, is_pressed);
}

// NEW: Expose the raw 16-bit hardware register
JSValue SpheroidScript::js_input_get_pad_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script) return JS_NewInt32(ctx, 0);

    int32_t port;
    JS_ToInt32(ctx, &port, argv[0]);

    if (port < 0 || port >= 4) return JS_NewInt32(ctx, 0);

    return JS_NewInt32(ctx, script->current_pad_state[port]);
}

JSValue SpheroidScript::js_apu_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    int32_t ch, reg, val;
    JS_ToInt32(ctx, &ch, argv[0]);
    JS_ToInt32(ctx, &reg, argv[1]);
    JS_ToInt32(ctx, &val, argv[2]);
    if (script && script->apu) script->apu->write(ch, reg, val);
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_apu_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    int32_t ch, reg;
    JS_ToInt32(ctx, &ch, argv[0]);
    JS_ToInt32(ctx, &reg, argv[1]);
    if (script && script->apu) return JS_NewInt32(ctx, script->apu->read(ch, reg));
    return JS_NewInt32(ctx, 0);
}

JSValue SpheroidScript::js_apu_write_global(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    int32_t reg, val;
    JS_ToInt32(ctx, &reg, argv[0]);
    JS_ToInt32(ctx, &val, argv[1]);
    if (script && script->apu) script->apu->writeGlobal(reg, val);
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_apu_read_global(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    int32_t reg;
    JS_ToInt32(ctx, &reg, argv[0]);
    if (script && script->apu) return JS_NewInt32(ctx, script->apu->readGlobal(reg));
    return JS_NewInt32(ctx, 0);
}