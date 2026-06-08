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
// ES6 Module Loader
// =============================================================================

JSModuleDef* SpheroidScript::module_loader(JSContext *ctx, const char *module_name, void *opaque) {
    SpheroidScript* script = static_cast<SpheroidScript*>(opaque);

    std::string req_path = module_name;
    if (req_path.substr(0, 2) == "./") req_path = req_path.substr(2);

    int fd = script->vfs->open(req_path.c_str(), "r");
    if (fd < 0) {
        if (script->logger) script->logger(RETRO_LOG_ERROR, "[MODULE] Could not find: %s\n", req_path.c_str());
        return nullptr;
    }
    
    script->vfs->seek(fd, 0, 2); 
    int64_t size = script->vfs->tell(fd);
    script->vfs->seek(fd, 0, 0); 

    std::vector<char> buffer(size + 1, '\0');
    script->vfs->read(fd, buffer.data(), size);
    script->vfs->close(fd);

    JSValue func_val = JS_Eval(ctx, buffer.data(), size, module_name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    
    if (JS_IsException(func_val)) {
        script->print_js_exception();
        return nullptr;
    }

    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);
    
    return m;
}

// Helper to reliably extract memory from any ArrayBuffer or TypedArray
static uint8_t* get_js_buffer(JSContext* ctx, JSValue val, size_t* out_size) {
    size_t size;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &size, val);
    if (ptr) {
        if (out_size) *out_size = size;
        return ptr;
    }
    // Fallback for Typed Arrays (Uint8Array, Float32Array, etc.)
    size_t byte_offset, byte_length;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &byte_offset, &byte_length, nullptr);
    if (!JS_IsException(ab)) {
        ptr = JS_GetArrayBuffer(ctx, &size, ab);
        JS_FreeValue(ctx, ab);
        if (ptr) {
            if (out_size) *out_size = byte_length;
            return ptr + byte_offset;
        }
    }
    if (out_size) *out_size = 0;
    return nullptr;
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

// =============================================================================
// UPDATED: System.fs.read(fd, buffer, length)
// =============================================================================
JSValue SpheroidScript::js_fs_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->vfs || argc < 3) return JS_NewInt32(ctx, 0);

    int32_t fd, length;
    JS_ToInt32(ctx, &fd, argv[0]);
    JS_ToInt32(ctx, &length, argv[2]);

    // Use our helper (from earlier) to extract the raw pointer from JS
    size_t buf_capacity;
    uint8_t* raw_ptr = get_js_buffer(ctx, argv[1], &buf_capacity);

    if (!raw_ptr || length <= 0 || (size_t)length > buf_capacity) {
        if (script->logger) script->logger(RETRO_LOG_WARN, "[VFS] Invalid JS buffer or size!\n");
        return JS_NewInt32(ctx, 0);
    }

    // Call your VFS! It writes directly into the JS garbage-collected memory.
    int64_t bytes_read = script->vfs->read(fd, raw_ptr, length);
    return JS_NewInt32(ctx, bytes_read > 0 ? (int32_t)bytes_read : 0);
}

// =============================================================================
// NEW: System.fs.readFile(path) -> Returns a new ArrayBuffer
// =============================================================================
JSValue SpheroidScript::js_fs_read_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->vfs || argc < 1) return JS_NULL;

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NULL;

    int fd = script->vfs->open(path, "r");
    JS_FreeCString(ctx, path);
    if (fd < 0) return JS_NULL; // File not found

    script->vfs->seek(fd, 0, 2); // SEEK_END
    int64_t size = script->vfs->tell(fd);
    script->vfs->seek(fd, 0, 0); // SEEK_SET

    if (size <= 0) {
        script->vfs->close(fd);
        return JS_NewArrayBufferCopy(ctx, nullptr, 0);
    }

    std::vector<uint8_t> buffer(size);
    script->vfs->read(fd, buffer.data(), size);
    script->vfs->close(fd);

    // Creates an ArrayBuffer in JS and copies the C++ vector into it!
    return JS_NewArrayBufferCopy(ctx, buffer.data(), size);
}

// =============================================================================
// System.fs.write(fd, buffer, length) (To support your save:// feature)
// =============================================================================
JSValue SpheroidScript::js_fs_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->vfs || argc < 3) return JS_NewInt32(ctx, 0);

    int32_t fd, length;
    JS_ToInt32(ctx, &fd, argv[0]);
    JS_ToInt32(ctx, &length, argv[2]);

    size_t buf_capacity;
    uint8_t* raw_ptr = get_js_buffer(ctx, argv[1], &buf_capacity);

    if (!raw_ptr || length <= 0 || (size_t)length > buf_capacity) return JS_NewInt32(ctx, 0);

    int64_t bytes_written = script->vfs->write(fd, raw_ptr, length);
    return JS_NewInt32(ctx, bytes_written > 0 ? (int32_t)bytes_written : 0);
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
// Core Implementation (Updated Initialization)
// =============================================================================

// --- NEW: Added SpheroidGPU* parameter ---
bool SpheroidScript::init(uint8_t* ram_ptr, size_t ram_size, VFSManager* vfs_mgr, SpheroidAPU* apu_ptr, SpheroidGPU* gpu_ptr, retro_log_printf_t log_cb) {
    logger = log_cb;
    vfs = vfs_mgr;
    apu = apu_ptr;
    gpu = gpu_ptr; // Store the GPU pointer
    system_ram = ram_ptr;
    system_ram_size = ram_size;

    rt = JS_NewRuntime();
    if (!rt) return false;

    ctx = JS_NewContext(rt);
    if (!ctx) return false;

    JS_SetContextOpaque(ctx, this);
    JS_SetModuleLoaderFunc(rt, nullptr, module_loader, this);

    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue system_obj = JS_NewObject(ctx);

    // 1. System.print
    JS_SetPropertyStr(ctx, system_obj, "print", JS_NewCFunction(ctx, js_print, "print", 1));

    // 2. System.fs
    JSValue fs_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs_obj, "open",  JS_NewCFunction(ctx, js_fs_open, "open", 2));
    JS_SetPropertyStr(ctx, fs_obj, "read",  JS_NewCFunction(ctx, js_fs_read, "read", 3));
	JS_SetPropertyStr(ctx, fs_obj, "readFile",  JS_NewCFunction(ctx, js_fs_read_file, "readFile", 3));
	JS_SetPropertyStr(ctx, fs_obj, "write",  JS_NewCFunction(ctx, js_fs_write, "write", 3));
    JS_SetPropertyStr(ctx, fs_obj, "seek",  JS_NewCFunction(ctx, js_fs_seek, "seek", 3));
    JS_SetPropertyStr(ctx, fs_obj, "tell",  JS_NewCFunction(ctx, js_fs_tell, "tell", 1));
    JS_SetPropertyStr(ctx, fs_obj, "close", JS_NewCFunction(ctx, js_fs_close, "close", 1));
    JS_SetPropertyStr(ctx, system_obj, "fs", fs_obj);

    // 3. System.audio
    JSValue audio_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, audio_obj, "load",      JS_NewCFunction(ctx, js_audio_load, "load", 1));
    JS_SetPropertyStr(ctx, audio_obj, "unload",    JS_NewCFunction(ctx, js_audio_unload, "unload", 1));
    JS_SetPropertyStr(ctx, audio_obj, "play",      JS_NewCFunction(ctx, js_audio_play, "play", 5));
    JS_SetPropertyStr(ctx, audio_obj, "stop",      JS_NewCFunction(ctx, js_audio_stop, "stop", 1));
    JS_SetPropertyStr(ctx, audio_obj, "stopAll",   JS_NewCFunction(ctx, js_audio_stop_all, "stopAll", 0));
    JS_SetPropertyStr(ctx, audio_obj, "setVolume", JS_NewCFunction(ctx, js_audio_set_volume, "setVolume", 2));
    JS_SetPropertyStr(ctx, audio_obj, "setPitch",  JS_NewCFunction(ctx, js_audio_set_pitch, "setPitch", 2));
    JS_SetPropertyStr(ctx, audio_obj, "setPan",    JS_NewCFunction(ctx, js_audio_set_pan, "setPan", 2));
    JS_SetPropertyStr(ctx, system_obj, "audio", audio_obj);

    // 4. System.input
    JSValue input_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, input_obj, "pressed",     JS_NewCFunction(ctx, js_input_pressed, "pressed", 2));
    JS_SetPropertyStr(ctx, input_obj, "getPadState", JS_NewCFunction(ctx, js_input_get_pad_state, "getPadState", 1));
    JS_SetPropertyStr(ctx, input_obj, "B", JS_NewInt32(ctx, 1<<0));
    JS_SetPropertyStr(ctx, input_obj, "UP", JS_NewInt32(ctx, 1<<4)); // (Add rest of inputs here later for brevity)
    JS_SetPropertyStr(ctx, system_obj, "input", input_obj);

    // =========================================================================
    // --- NEW: System.gpu Bindings ---
    // =========================================================================
    JSValue gpu_obj = JS_NewObject(ctx);
    
    // Resources
    JS_SetPropertyStr(ctx, gpu_obj, "loadTexture", JS_NewCFunction(ctx, js_gpu_load_texture, "loadTexture", 3));
    JS_SetPropertyStr(ctx, gpu_obj, "loadMesh", JS_NewCFunction(ctx, js_gpu_load_mesh, "loadMesh", 2));
    
    // Command Queue
    JS_SetPropertyStr(ctx, gpu_obj, "clear", JS_NewCFunction(ctx, js_gpu_cmd_clear, "clear", 1));
    JS_SetPropertyStr(ctx, gpu_obj, "setRenderList", JS_NewCFunction(ctx, js_gpu_cmd_set_render_list, "setRenderList", 1));
    JS_SetPropertyStr(ctx, gpu_obj, "pushMatrix", JS_NewCFunction(ctx, js_gpu_cmd_push_matrix, "pushMatrix", 0));
    JS_SetPropertyStr(ctx, gpu_obj, "popMatrix", JS_NewCFunction(ctx, js_gpu_cmd_pop_matrix, "popMatrix", 0));
    JS_SetPropertyStr(ctx, gpu_obj, "loadIdentity", JS_NewCFunction(ctx, js_gpu_cmd_load_identity, "loadIdentity", 0));
    JS_SetPropertyStr(ctx, gpu_obj, "loadMatrix", JS_NewCFunction(ctx, js_gpu_cmd_load_matrix, "loadMatrix", 1));
    JS_SetPropertyStr(ctx, gpu_obj, "translate", JS_NewCFunction(ctx, js_gpu_cmd_translate, "translate", 3));
    JS_SetPropertyStr(ctx, gpu_obj, "rotateX", JS_NewCFunction(ctx, js_gpu_cmd_rotate_x, "rotateX", 1));
    JS_SetPropertyStr(ctx, gpu_obj, "rotateY", JS_NewCFunction(ctx, js_gpu_cmd_rotate_y, "rotateY", 1));
    JS_SetPropertyStr(ctx, gpu_obj, "rotateZ", JS_NewCFunction(ctx, js_gpu_cmd_rotate_z, "rotateZ", 1));
    JS_SetPropertyStr(ctx, gpu_obj, "scale", JS_NewCFunction(ctx, js_gpu_cmd_scale, "scale", 3));
    JS_SetPropertyStr(ctx, gpu_obj, "setProjection", JS_NewCFunction(ctx, js_gpu_cmd_set_projection, "setProjection", 4));
    JS_SetPropertyStr(ctx, gpu_obj, "drawMesh", JS_NewCFunction(ctx, js_gpu_cmd_draw_mesh, "drawMesh", 2));

    JS_SetPropertyStr(ctx, system_obj, "gpu", gpu_obj);

    // 5. Expose RAM ArrayBuffer
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

// =============================================================================
// Native JS GPU API (NEW)
// =============================================================================

JSValue SpheroidScript::js_gpu_load_texture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->gpu || argc < 3) return JS_NewInt32(ctx, 0);

    uint32_t w, h;
    JS_ToUint32(ctx, &w, argv[0]);
    JS_ToUint32(ctx, &h, argv[1]);

    size_t size;
    uint8_t* buf = get_js_buffer(ctx, argv[2], &size);
    if (!buf || size < (w * h * 4)) return JS_NewInt32(ctx, 0); // Need RGBA8888 buffer

    uint32_t id = script->gpu->load_texture(w, h, (const uint32_t*)buf);
    return JS_NewInt32(ctx, id);
}

JSValue SpheroidScript::js_gpu_load_mesh(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->gpu || argc < 2) return JS_NewInt32(ctx, 0);

    size_t v_len, i_len;
    uint8_t* v_buf = get_js_buffer(ctx, argv[0], &v_len);
    uint8_t* i_buf = get_js_buffer(ctx, argv[1], &i_len);

    if (!v_buf || !i_buf || v_len % sizeof(GPUVertex) != 0 || i_len % sizeof(uint16_t) != 0) {
        return JS_NewInt32(ctx, 0);
    }

    std::vector<GPUVertex> verts((GPUVertex*)v_buf, (GPUVertex*)(v_buf + v_len));
    std::vector<uint16_t> indices((uint16_t*)i_buf, (uint16_t*)(i_buf + i_len));

    uint32_t id = script->gpu->load_mesh(verts, indices);
    return JS_NewInt32(ctx, id);
}

JSValue SpheroidScript::js_gpu_cmd_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc > 0) {
        uint32_t color; JS_ToUint32(ctx, &color, argv[0]);
        script->gpu->cmd_clear(color);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_set_render_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc > 0) {
        int32_t type; JS_ToInt32(ctx, &type, argv[0]);
        script->gpu->cmd_set_render_list(static_cast<RenderListType>(type));
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_push_matrix(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu) script->gpu->cmd_push_matrix();
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_pop_matrix(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu) script->gpu->cmd_pop_matrix();
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_load_identity(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu) script->gpu->cmd_load_identity();
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_load_matrix(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc > 0) {
        size_t size;
        uint8_t* buf = get_js_buffer(ctx, argv[0], &size);
        if (buf && size >= 16 * sizeof(float)) {
            HMM_Mat4 mat;
            std::memcpy(&mat.Elements[0][0], buf, 16 * sizeof(float));
            script->gpu->cmd_load_matrix(mat);
        }
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_translate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc >= 3) {
        double x, y, z;
        JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]); JS_ToFloat64(ctx, &z, argv[2]);
        script->gpu->cmd_translate((float)x, (float)y, (float)z);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_rotate_x(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc > 0) {
        double angle; JS_ToFloat64(ctx, &angle, argv[0]);
        script->gpu->cmd_rotate_x((float)angle);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_rotate_y(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc > 0) {
        double angle; JS_ToFloat64(ctx, &angle, argv[0]);
        script->gpu->cmd_rotate_y((float)angle);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_rotate_z(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc > 0) {
        double angle; JS_ToFloat64(ctx, &angle, argv[0]);
        script->gpu->cmd_rotate_z((float)angle);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_scale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc >= 3) {
        double x, y, z;
        JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]); JS_ToFloat64(ctx, &z, argv[2]);
        script->gpu->cmd_scale((float)x, (float)y, (float)z);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_set_projection(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc >= 4) {
        double fov, aspect, nearZ, farZ;
        JS_ToFloat64(ctx, &fov, argv[0]); JS_ToFloat64(ctx, &aspect, argv[1]); 
        JS_ToFloat64(ctx, &nearZ, argv[2]); JS_ToFloat64(ctx, &farZ, argv[3]);
        script->gpu->cmd_set_projection((float)fov, (float)aspect, (float)nearZ, (float)farZ);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_gpu_cmd_draw_mesh(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->gpu && argc >= 2) {
        uint32_t meshId, texId;
        JS_ToUint32(ctx, &meshId, argv[0]);
        JS_ToUint32(ctx, &texId, argv[1]);
        script->gpu->cmd_draw_mesh(meshId, texId);
    }
    return JS_UNDEFINED;
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

// =============================================================================
// Native JS Audio API (High-Level)
// =============================================================================

JSValue SpheroidScript::js_audio_load(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->apu || argc < 1) return JS_NewInt32(ctx, -1);

    const char* filepath = JS_ToCString(ctx, argv[0]);
    if (!filepath) return JS_NewInt32(ctx, -1);

    int id = script->apu->load_sound(filepath);
    JS_FreeCString(ctx, filepath);

    return JS_NewInt32(ctx, id);
}

JSValue SpheroidScript::js_audio_unload(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->apu || argc < 1) return JS_UNDEFINED;

    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0) {
        script->apu->unload_sound(id);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_audio_play(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->apu || argc < 1) return JS_NewInt32(ctx, -1);

    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);

    // Optional parameters with safe defaults
    double volume = 1.0, pitch = 1.0, pan = 0.0;
    bool loop = false;

    if (argc > 1 && !JS_IsUndefined(argv[1])) JS_ToFloat64(ctx, &volume, argv[1]);
    if (argc > 2 && !JS_IsUndefined(argv[2])) JS_ToFloat64(ctx, &pitch, argv[2]);
    if (argc > 3 && !JS_IsUndefined(argv[3])) JS_ToFloat64(ctx, &pan, argv[3]);
    if (argc > 4 && !JS_IsUndefined(argv[4])) loop = JS_ToBool(ctx, argv[4]);

    int voice_id = script->apu->play(id, (float)volume, (float)pitch, (float)pan, loop);
    return JS_NewInt32(ctx, voice_id);
}

JSValue SpheroidScript::js_audio_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (!script || !script->apu || argc < 1) return JS_UNDEFINED;
    
    int32_t voice_id;
    if (JS_ToInt32(ctx, &voice_id, argv[0]) == 0) script->apu->stop(voice_id);
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_audio_stop_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->apu) script->apu->stop_all();
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_audio_set_volume(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->apu && argc >= 2) {
        int32_t voice_id; double vol;
        JS_ToInt32(ctx, &voice_id, argv[0]); JS_ToFloat64(ctx, &vol, argv[1]);
        script->apu->set_volume(voice_id, (float)vol);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_audio_set_pitch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->apu && argc >= 2) {
        int32_t voice_id; double pitch;
        JS_ToInt32(ctx, &voice_id, argv[0]); JS_ToFloat64(ctx, &pitch, argv[1]);
        script->apu->set_pitch(voice_id, (float)pitch);
    }
    return JS_UNDEFINED;
}

JSValue SpheroidScript::js_audio_set_pan(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    SpheroidScript* script = static_cast<SpheroidScript*>(JS_GetContextOpaque(ctx));
    if (script && script->apu && argc >= 2) {
        int32_t voice_id; double pan;
        JS_ToInt32(ctx, &voice_id, argv[0]); JS_ToFloat64(ctx, &pan, argv[1]);
        script->apu->set_pan(voice_id, (float)pan);
    }
    return JS_UNDEFINED;
}