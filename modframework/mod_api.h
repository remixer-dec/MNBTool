#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MF_NATIVE_ABI_VERSION 1u

typedef enum mf_result_v1 {
    MF_OK = 0,
    MF_ERROR_INVALID_ARGUMENT = -1,
    MF_ERROR_DENIED = -2,
    MF_ERROR_CONFLICT = -3,
    MF_ERROR_NOT_FOUND = -4,
    MF_ERROR_HOOK_FAILED = -5,
    MF_ERROR_PATCH_MISMATCH = -6,
    MF_ERROR_IO = -7,
} mf_result_v1;

typedef enum mf_hook_behavior_v1 {
    MF_HOOK_CHAIN = 0,
    MF_HOOK_REPLACE = 1,
    MF_HOOK_EXCLUSIVE = 2,
} mf_hook_behavior_v1;

typedef enum mf_log_priority_v1 {
    MF_LOG_DEBUG = 3,
    MF_LOG_INFO = 4,
    MF_LOG_WARNING = 5,
    MF_LOG_ERROR = 6,
} mf_log_priority_v1;

typedef enum mf_storage_scope_v1 {
    MF_STORAGE_MOD = 0,
    MF_STORAGE_GAME = 1,
} mf_storage_scope_v1;

typedef enum mf_memory_access_v1 {
    MF_MEMORY_READ = 1,
    MF_MEMORY_WRITE = 2,
    MF_MEMORY_EXECUTE = 4,
} mf_memory_access_v1;

#define MF_JAVA_MESSAGE_MAX_BYTES (64u * 1024u)
#define MF_VIBRATION_MAX_MILLISECONDS 1000u
#define MF_LAUNCHER_CONFIG_GAME_MODULE_DIRECTORY "game.module.directory"
#define MF_LAUNCHER_CONFIG_GAME_LANGUAGE "game.language"
#define MF_LAUNCHER_CONFIG_DISPLAY_HDR "display.hdr"

/* mf_motion_v1.sensors: which readings are present. */
#define MF_MOTION_GYROSCOPE 1u
#define MF_MOTION_ACCELEROMETER 2u

/*
 * The device's newest motion readings, in its own axes (natural orientation: x right, y up,
 * z out of the screen; the game runs in landscape, so a phone's x points along the screen's
 * height). Set struct_size before the call.
 */
typedef struct mf_motion_v1 {
    uint32_t struct_size;
    uint32_t sensors;          /* MF_MOTION_* bits; 0 until the first readings arrive */
    float rotation[3];         /* gyroscope, radians per second about x, y, z */
    float acceleration[3];     /* accelerometer, m/s^2, gravity included */
    int64_t timestamp_ns;      /* of the newest reading (the sensors' clock) */
} mf_motion_v1;

typedef struct mf_mod_context_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* mod_id;
    const char* mod_version;
    const char* package_sha256;
    const char* game_build_id;
    uint32_t reserved; /* ABI v1 compatibility slot; do not read or write. */
    void* runtime_handle;
} mf_mod_context_v1;

typedef struct mf_import_hook_v1 {
    uint32_t struct_size;
    mf_hook_behavior_v1 behavior;
    const char* hook_id;
    const char* caller_library;
    const char* callee_library;
    const char* symbol;
    void* replacement;
    void** previous;
} mf_import_hook_v1;

typedef struct mf_function_hook_v1 {
    uint32_t struct_size;
    mf_hook_behavior_v1 behavior;
    const char* hook_id;
    const char* target_library;
    const char* symbol; /* Exactly one of symbol or offset must be supplied. */
    uintptr_t offset;
    void* replacement;
    void** original;
} mf_function_hook_v1;

/* Source ABI, independent of the native SDK/object architecture. */
#define MF_GAME_ABI_ARM32_BASE 1u
typedef uint64_t (*mf_game_core_function_v1)(uint32_t, uint32_t, uint32_t, uint32_t);

/*
 * Compiler-produced typed hook binding. Automatic builds emit this record and
 * route the ordinary request through hook_function_compatible. An author who
 * manages their own ARM64 variant may supply it explicitly. The record and all
 * function/slot addresses belong to the requesting object. Native SDK pointers
 * remain native-sized; previous_core holds a source-ABI callable, while
 * native_original is the typed native thunk published into request.original.
 */
typedef struct mf_game_hook_adapter_v1 {
    uint32_t struct_size;
    uint32_t source_abi;
    void* native_replacement;
    mf_game_core_function_v1 core_entry;
    void** previous_core;
    void* native_original;
} mf_game_hook_adapter_v1;

/*
 * Checked source-frame services used by generated/public typed helpers. decode_pointer with
 * size 0 and access 0 converts like a C cast, without checking the memory; encode_pointer
 * accepts any pointer into the game's memory.
 */
typedef struct mf_game_services_v1 {
    uint32_t struct_size;
    uint32_t source_abi;
    mf_result_v1 (*read_stack)(const mf_mod_context_v1*, uint32_t index, uint32_t* value);
    mf_result_v1 (*write_stack)(const mf_mod_context_v1*, uint32_t index, uint32_t value);
    void* (*decode_pointer)(const mf_mod_context_v1*, uint32_t address, size_t size, uint32_t access);
    mf_result_v1 (*encode_pointer)(const mf_mod_context_v1*, const void* pointer, uint32_t* address);
    mf_result_v1 (*call_function)(const mf_mod_context_v1*, const char* library, uint64_t offset,
                                  const uint32_t* words, size_t word_count, uint64_t* result);
} mf_game_services_v1;

/*
 * The game functions a package calls through resolve_game_symbol or resolve_game_address
 * (MF_MEMORY_EXECUTE), with their ARMv7 prototypes. Automatic ARM64 builds generate this
 * table (exported as MF_GAME_FUNCTIONS_SYMBOL); authors never write it. On the translated
 * game those calls return a callable adapter for each listed function. `signature` is the
 * result, then each argument, one letter each: v void, b bool, c/C 8-bit, s/S 16-bit,
 * i/I 32-bit, l/L 64-bit (lower case signed), f float, d double, p pointer. A zero entry ends it.
 */
typedef struct mf_game_function_v1 {
    const char* library;
    const char* symbol; /* NULL for an offset */
    uint64_t offset;
    const char* signature;
} mf_game_function_v1;
#define MF_GAME_FUNCTIONS_SYMBOL "mf_game_functions_v1"
/*
 * void* mf_game_pointer_v1(unsigned long long address): the pointer for a game address
 * (0 stays null). Automatic ARM64 builds call it where package code converts a 32-bit
 * integer to a pointer; the translated game's launcher provides it.
 */
#define MF_GAME_POINTER_SYMBOL "mf_game_pointer_v1"

typedef struct mf_byte_patch_v1 {
    uint32_t struct_size;
    mf_hook_behavior_v1 behavior;
    const char* hook_id;
    const char* target_library;
    uintptr_t offset;
    const void* expected_bytes;
    const void* replacement_bytes;
    size_t byte_count;
} mf_byte_patch_v1;

/*
 * Templates (render_template, and the texts of edit_text), e.g. GLSL:
 *   {{ name }}              the value: true/false, an integer, a float as a GLSL literal (always
 *                           with a '.'), or a string as it is; negative numbers in parentheses
 *   {% if name %} ... {% elif name %} ... {% else %} ... {% endif %}
 *                           keeps the branch whose BOOL value is true; "if not name" negates
 *   {% for item in list %} ... {% endfor %}
 *                           repeats for each item of a LIST value, `item` being that item
 *   {% for i in count %} ... {% endfor %}
 *                           repeats with i = 0 .. count - 1; count is an INT value or a number
 * A line holding only a {% %} tag is removed whole. Every name, in every branch and loop, must be
 * a value; an unknown or reused name, a condition that is not a BOOL, a malformed tag or more
 * than 1024 repeats is an error. "{{" and "{%" always open a tag.
 */
typedef enum mf_value_type_v1 {
    MF_VALUE_BOOL = 0,
    MF_VALUE_INT = 1,
    MF_VALUE_FLOAT = 2,
    MF_VALUE_STRING = 3,
    MF_VALUE_LIST = 4,
} mf_value_type_v1;

typedef struct mf_template_value_v1 {
    const char* name; /* letters, digits and '_', not starting with a digit; ignored in a list */
    mf_value_type_v1 type;
    int32_t integer;  /* BOOL (0 or 1) and INT */
    float decimal;    /* FLOAT, finite */
    const char* text; /* STRING */
    const struct mf_template_value_v1* items; /* LIST: BOOL, INT, FLOAT or STRING items, one type */
    size_t item_count;                        /* LIST */
} mf_template_value_v1;

#define MF_BOOL(name, value) {(name), MF_VALUE_BOOL, (value) ? 1 : 0, 0.0f, NULL, NULL, 0}
#define MF_INT(name, value) {(name), MF_VALUE_INT, (int32_t)(value), 0.0f, NULL, NULL, 0}
#define MF_FLOAT(name, value) {(name), MF_VALUE_FLOAT, 0, (float)(value), NULL, NULL, 0}
#define MF_STRING(name, value) {(name), MF_VALUE_STRING, 0, 0.0f, (value), NULL, 0}
#define MF_LIST(name, items, count) {(name), MF_VALUE_LIST, 0, 0.0f, NULL, (items), (count)}

/*
 * Text edits (edit_text) apply in order, each to the previous one's result. An anchor is literal
 * text that may hold captures: {{ name }} matches the shortest text, its brackets balanced and
 * no ';' outside them, that the anchor's next literal text follows; the edit's template reads it
 * as a STRING. BEFORE, AFTER and REPLACE need their anchor exactly once, REPLACE_ALL at least once.
 */
typedef enum mf_text_edit_op_v1 {
    MF_EDIT_REQUIRE = 0,     /* the anchor must be present; no text */
    MF_EDIT_BEFORE = 1,      /* inserts the text before the anchor */
    MF_EDIT_AFTER = 2,       /* inserts the text after the anchor */
    MF_EDIT_REPLACE = 3,     /* replaces the anchor with the text */
    MF_EDIT_REPLACE_ALL = 4, /* replaces every occurrence */
    MF_EDIT_PREPEND = 5,     /* inserts the text at the start; no anchor */
    MF_EDIT_APPEND = 6,      /* adds the text at the end; no anchor */
} mf_text_edit_op_v1;

/* Or-ed into an op: a missing anchor skips this edit instead of failing them all. */
#define MF_EDIT_OPTIONAL 0x100u

typedef struct mf_text_edit_v1 {
    uint32_t op;
    const char* anchor;
    const char* text; /* a template */
} mf_text_edit_v1;

/* The element count of an array, e.g. edit_text(context, source, edits, MF_COUNT(edits), ...). */
#define MF_COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef struct mf_api_v1 {
    uint32_t struct_size;
    uint32_t abi_version;

    int (*is_mod_active)(const mf_mod_context_v1* context, const char* mod_id);
    const char* (*get_mod_version)(const mf_mod_context_v1* context, const char* mod_id);
    void (*log)(const mf_mod_context_v1* context, mf_log_priority_v1 priority,
                const char* message);

    mf_result_v1 (*hook_import)(const mf_mod_context_v1* context,
                                const mf_import_hook_v1* request);
    mf_result_v1 (*hook_function)(const mf_mod_context_v1* context,
                                  const mf_function_hook_v1* request);
    mf_result_v1 (*patch_bytes)(const mf_mod_context_v1* context,
                                const mf_byte_patch_v1* request);
    void* (*resolve_game_symbol)(const mf_mod_context_v1* context,
                                 const char* library, const char* symbol);
    void* (*resolve_game_address)(const mf_mod_context_v1* context,
                                  const char* library, uintptr_t offset,
                                  size_t byte_count, uint32_t required_access);

    int (*storage_open)(const mf_mod_context_v1* context, mf_storage_scope_v1 scope,
                        const char* relative_path, int flags);
    intptr_t (*storage_read)(const mf_mod_context_v1* context, int descriptor,
                             void* buffer, size_t byte_count);
    intptr_t (*storage_write)(const mf_mod_context_v1* context, int descriptor,
                              const void* buffer, size_t byte_count);
    int64_t (*storage_seek)(const mf_mod_context_v1* context, int descriptor,
                            int64_t offset, int whence);
    mf_result_v1 (*storage_close)(const mf_mod_context_v1* context, int descriptor);

    /*
     * Posts an opaque message to this package's JavaMod component. Delivery occurs on
     * Android's main thread. Returns MF_ERROR_DENIED when the package has no Java component.
     */
    mf_result_v1 (*post_java_message)(const mf_mod_context_v1* context,
                                      const void* message, size_t byte_count);

    /* Returns a process-lifetime value, or NULL when the launcher did not publish the key. */
    const char* (*get_launcher_config)(const mf_mod_context_v1* context,
                                       const char* key);

    /*
     * Returns the address of a global symbol that another active mod (packaged or built-in)
     * lists in its manifest "exports", or NULL. The caller must declare the pair in its manifest
     * "imports". Every object is mapped before any mod_start runs, and an active provider
     * starts before its importers. Check struct_size before use on older launchers.
     */
    void* (*resolve_mod_symbol)(const mf_mod_context_v1* context, const char* provider_mod_id,
                                const char* symbol);

    /*
     * Superseded by the typed get_setting_* getters at the end of the table; kept in place for
     * mods built before them. Returns a setting as text, or NULL for an undeclared key.
     */
    const char* (*get_setting_text)(const mf_mod_context_v1* context, const char* key);

    /*
     * Returns an OpenGL ES or EGL entry point by name ("gl" or "egl" followed by an uppercase
     * letter), core or extension, or NULL. The game's context decides what may be called:
     * check its version and extension strings before using anything beyond ES 2.0.
     */
    void* (*resolve_gl_function)(const mf_mod_context_v1* context, const char* name);

    /*
     * This mod's manifest-declared settings, typed as declared (BOOL, INT, FLOAT, STRING) and
     * fixed for the game process: the player's value or the manifest default. For a key that is
     * undeclared or declared with another type, a getter returns `fallback` and the launcher
     * logs the mistake. Strings stay valid for the process lifetime.
     */
    int (*get_setting_bool)(const mf_mod_context_v1* context, const char* key, int fallback);
    int32_t (*get_setting_int)(const mf_mod_context_v1* context, const char* key, int32_t fallback);
    float (*get_setting_float)(const mf_mod_context_v1* context, const char* key, float fallback);
    const char* (*get_setting_string)(const mf_mod_context_v1* context, const char* key,
                                      const char* fallback);

    /*
     * Renders a template (see mf_template_value_v1) and returns the text, which the caller
     * releases with free(), or NULL after logging the template's mistake.
     */
    char* (*render_template)(const mf_mod_context_v1* context, const char* text,
                             const mf_template_value_v1* values, size_t value_count);

    /*
     * Applies `edits` (see mf_text_edit_v1) to a copy of `source` and returns it, to release with
     * free(). NULL means nothing changed: an anchor is missing or repeated (not logged: the text
     * is not the one the edits expect), no edit applied, or a mistake in the edits (logged).
     */
    char* (*edit_text)(const mf_mod_context_v1* context, const char* source,
                       const mf_text_edit_v1* edits, size_t edit_count,
                       const mf_template_value_v1* values, size_t value_count);

    /* Appended ABI-v1 services: callers must check MF_API_HAS. */
    mf_result_v1 (*hook_function_compatible)(const mf_mod_context_v1* context,
                                            const mf_function_hook_v1* request,
                                            const mf_game_hook_adapter_v1* adapter);
    /* Synchronous native callback execution through the framework's bound
     * executor. The callback belongs to this mod; argument lifetime belongs to
     * the synchronous caller. No private framework headers/imports are needed. */
    mf_result_v1 (*invoke_callback)(const mf_mod_context_v1* context,
                                    void (*callback)(void*), void* argument);
    const mf_game_services_v1* (*get_game_services)(const mf_mod_context_v1* context);

    /*
     * Vibrates the device for `milliseconds` (1 to MF_VIBRATION_MAX_MILLISECONDS) at `strength`
     * (1 to 255; devices without amplitude control have one strength). A call replaces a running
     * vibration. MF_ERROR_NOT_FOUND: the device cannot vibrate. Any thread.
     */
    mf_result_v1 (*vibrate)(const mf_mod_context_v1* context, uint32_t milliseconds,
                            uint32_t strength);
    /*
     * Fills `motion` with the newest gyroscope and accelerometer readings. The first call in a
     * game process starts the sensors (about 60 readings a second from then on), so its
     * readings are still missing. MF_ERROR_NOT_FOUND: the device has neither sensor. Any thread.
     */
    mf_result_v1 (*read_motion)(const mf_mod_context_v1* context, mf_motion_v1* motion);
} mf_api_v1;

/*
 * Whether the launcher's table has a function. Within ABI v1 the table only grows (new
 * functions are appended), so a launcher older than this header may lack the last ones:
 * if (MF_API_HAS(api, get_setting_int)) ...
 */
#define MF_API_HAS(api, function) \
    ((api)->struct_size >= offsetof(mf_api_v1, function) + sizeof((api)->function))

typedef mf_result_v1 (*mf_mod_start_v1)(const mf_api_v1* api,
                                        const mf_mod_context_v1* context);

/*
 * Defines the entry point: MF_MOD_START(my_mod)(const mf_api_v1* api,
 * const mf_mod_context_v1* context) { ... }. A package exports it as mod_start. Compiled
 * into the launcher as a hosted built-in (MF_HOSTED_BUILT_IN), it is my_mod_mod_start.
 */
#ifdef __cplusplus
#define MF_EXTERN_C extern "C"
#else
#define MF_EXTERN_C
#endif
#if defined(MF_HOSTED_BUILT_IN)
#define MF_MOD_START(name) MF_EXTERN_C mf_result_v1 name##_mod_start
#else
#define MF_MOD_START(name) MF_EXTERN_C mf_result_v1 mod_start
#endif
typedef void (*mf_mod_stop_v1)(void);
/* Optional export used by JavaModContext.sendToNative; invoked synchronously. */
typedef void (*mf_java_message_v1)(const void* message, size_t byte_count);

#ifdef __cplusplus
}
#endif
