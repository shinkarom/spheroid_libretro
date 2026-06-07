#include "script.hpp"
#include <vector>

// =============================================================================
// QuickJS C-Bindings
// =============================================================================

JSValue SpheroidScript::js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    // Retrieve our C++ class instance from the QuickJS context
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
// Core Implementation
// =============================================================================

bool SpheroidScript::init(uint8_t* ram_ptr, size_t ram_size, retro_log_printf_t log_cb) {
    logger = log_cb;

    rt = JS_NewRuntime();
    if (!rt) {
        if (logger) logger(RETRO_LOG_ERROR, "Failed to create QuickJS Runtime\n");
        return false;
    }

    ctx = JS_NewContext(rt);
    if (!ctx) {
        if (logger) logger(RETRO_LOG_ERROR, "Failed to create QuickJS Context\n");
        return false;
    }

    // Store 'this' so static C callbacks can access our class variables
    JS_SetContextOpaque(ctx, this);

    // Create a global 'System' object
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue system_obj = JS_NewObject(ctx);

    // 1. Bind console log function
    JS_SetPropertyStr(ctx, system_obj, "print", JS_NewCFunction(ctx, js_print, "print", 1));

    // 2. The Magic Trick: Expose 32MB C++ array directly to JS as an ArrayBuffer!
    // We use dummy_free_ram so QuickJS doesn't try to free our C++ allocated memory.
    JSValue ram_buffer = JS_NewArrayBuffer(ctx, ram_ptr, ram_size, dummy_free_ram, nullptr, false);
    JS_SetPropertyStr(ctx, system_obj, "RAM", ram_buffer);

    // Apply 'System' to the global scope
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
    // Evaluate the game script
    JSValue eval_res = JS_Eval(ctx, js_source, source_size, "rom.js", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(eval_res)) {
        print_js_exception();
        JS_FreeValue(ctx, eval_res);
        return false;
    }
    JS_FreeValue(ctx, eval_res);

    // Cache references to the game's init() and update() functions
    JSValue global_obj = JS_GetGlobalObject(ctx);
    js_init_func = JS_GetPropertyStr(ctx, global_obj, "init");
    js_update_func = JS_GetPropertyStr(ctx, global_obj, "update");
    JS_FreeValue(ctx, global_obj);

    if (!JS_IsFunction(ctx, js_update_func)) {
        if (logger) logger(RETRO_LOG_ERROR, "JS ROM must export an 'update()' function!\n");
        return false;
    }

    return true;
}

void SpheroidScript::call_init() {
    if (JS_IsFunction(ctx, js_init_func)) {
        JSValue result = JS_Call(ctx, js_init_func, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(result)) {
            print_js_exception();
        }
        JS_FreeValue(ctx, result);
    }
}

void SpheroidScript::call_update() {
    // 1. Call the main game update() function
    if (JS_IsFunction(ctx, js_update_func)) {
        JSValue result = JS_Call(ctx, js_update_func, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(result)) {
            print_js_exception();
        }
        JS_FreeValue(ctx, result);
    }

    // 2. Flush the Async/Promise Microtask Queue
    JSContext *pctx;
    int err;
    for (;;) {
        // Execute the next pending job. Returns 1 if a job was executed, 
        // 0 if the queue is empty, and < 0 on exception.
        err = JS_ExecutePendingJob(rt, &pctx);
        if (err <= 0) {
            if (err < 0) {
                // If an async function threw an unhandled exception, catch it here!
                print_js_exception(); 
            }
            break; // Queue is empty, exit loop
        }
    }
}