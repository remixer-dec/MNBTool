// Frame Rate Log: a small example of how a native mod is written.
//
// - One source file, compiled freestanding: the only headers are the ones
//   mod_api.h brings (<stddef.h>, <stdint.h>). The few C functions the launcher
//   provides (see README.md) are declared directly.
// - State is plain static data, zero or constant initialised: no global
//   constructors, exceptions, RTTI or thread-locals.
// - Everything goes through mf_api_v1, and each hook request repeats a
//   declaration in manifest.json (same id, behaviour, libraries and symbol).
// - A CHAIN hook always calls the previous function, read at each call: other
//   mods' hooks on the same import are linked through it.
// - Text with values is a template the launcher renders, never pieces joined
//   in C (there is no snprintf); shader code is written the same way.
#include "mod_api.h"

namespace {

// <time.h> does not build freestanding; clock_gettime is provided by the launcher.
struct Timespec {
    long seconds;
    long nanoseconds;
};
extern "C" int clock_gettime(int clock, Timespec* time);
extern "C" void free(void* pointer);
constexpr int kClockMonotonic = 1;

constexpr unsigned kGlRenderer = 0x1F01;  // GL_RENDERER
constexpr int32_t kDefaultIntervalSeconds = 5;

constexpr char kGpuLine[] = "GPU: {{ gpu }}";
constexpr char kRateLine[] = "frame rate: {{ fps }} fps{% if slow %} (below 30){% endif %}";

using SwapBuffers = unsigned (*)(void* display, void* surface);  // eglSwapBuffers
using GetString = const unsigned char* (*)(unsigned name);      // glGetString

const mf_api_v1* g_api = nullptr;
const mf_mod_context_v1* g_context = nullptr;
SwapBuffers g_previous_swap = nullptr;  // set by the framework
GetString g_get_string = nullptr;
unsigned g_interval_ms = kDefaultIntervalSeconds * 1000u;
unsigned g_window_start_ms = 0;
unsigned g_frames = 0;
bool g_gpu_logged = false;

// 32-bit arithmetic only: the launcher has no helper for 64-bit division.
// The millisecond count wraps after 49 days; differences stay correct.
unsigned now_ms() {
    Timespec time{};
    clock_gettime(kClockMonotonic, &time);
    return static_cast<unsigned>(time.seconds) * 1000u + static_cast<unsigned>(time.nanoseconds) / 1000000u;
}

// Logs a line template filled in with `values`. The rendered text is ours to free.
void log_line(const char* line, const mf_template_value_v1* values, size_t count) {
    char* const text = g_api->render_template(g_context, line, values, count);
    if (text != nullptr) g_api->log(g_context, MF_LOG_INFO, text);
    free(text);
}

// The game calls eglSwapBuffers on its render thread at the end of each frame,
// with its GL context current, so GL may be called here.
unsigned replacement_swap(void* display, void* surface) {
    if (!g_gpu_logged) {
        g_gpu_logged = true;
        const unsigned char* gpu = g_get_string(kGlRenderer);
        if (gpu != nullptr) {
            const mf_template_value_v1 values[] = {MF_STRING("gpu", reinterpret_cast<const char*>(gpu))};
            log_line(kGpuLine, values, MF_COUNT(values));
        }
    }

    ++g_frames;
    const unsigned now = now_ms();
    const unsigned elapsed = now - g_window_start_ms;
    if (elapsed >= g_interval_ms) {
        const unsigned fps = g_frames * 1000u / elapsed;
        const mf_template_value_v1 values[] = {MF_INT("fps", fps), MF_BOOL("slow", fps < 30u)};
        log_line(kRateLine, values, MF_COUNT(values));
        g_frames = 0;
        g_window_start_ms = now;
    }
    return g_previous_swap(display, surface);
}

}  // namespace

MF_MOD_START(frame_rate_log)(const mf_api_v1* api, const mf_mod_context_v1* context) {
    // The table only grows (see README.md, Compatibility): render_template, the newest
    // function used here, being there means every function before it is too.
    if (api == nullptr || context == nullptr || api->abi_version != MF_NATIVE_ABI_VERSION ||
        !MF_API_HAS(api, render_template)) {
        return MF_ERROR_INVALID_ARGUMENT;
    }
    g_api = api;
    g_context = context;
    // The manifest declares "interval" as INT 1..60, so the value is already in range.
    g_interval_ms = static_cast<unsigned>(api->get_setting_int(context, "interval", kDefaultIntervalSeconds)) * 1000u;
    g_get_string = reinterpret_cast<GetString>(api->resolve_gl_function(context, "glGetString"));
    if (g_get_string == nullptr) return MF_ERROR_NOT_FOUND;
    g_window_start_ms = now_ms();

    mf_import_hook_v1 request{};
    request.struct_size = sizeof(request);
    request.behavior = MF_HOOK_CHAIN;
    request.hook_id = "frame-end";
    request.caller_library = "libMBExpMobile.so";
    request.callee_library = "libEGL.so";
    request.symbol = "eglSwapBuffers";
    request.replacement = reinterpret_cast<void*>(&replacement_swap);
    request.previous = reinterpret_cast<void**>(&g_previous_swap);
    const mf_result_v1 result = api->hook_import(context, &request);
    if (result != MF_OK) {
        api->log(context, MF_LOG_ERROR, "frame hook refused");
        return result;
    }
    api->log(context, MF_LOG_INFO, "active");
    return MF_OK;
}
