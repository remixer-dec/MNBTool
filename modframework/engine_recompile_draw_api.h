#pragma once

#include "engine_recompile_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mb_draw_elements_fn)(unsigned mode, int count, unsigned type, const void* indices);
typedef int (*mb_record_needs_draw_fn)(const mb_record_draw* draw);

// A draw import that only accounts for draws while idle. The engine can then
// submit through the verified GLES continuation and report the logical count.
// All callbacks run on the GL thread. next() reports the live continuation.
typedef struct mb_draw_counter_link {
    uint32_t struct_size;
    mb_draw_elements_fn entry;
    mb_draw_elements_fn (*next)(void);
    int (*count_only)(void);
    void (*add_draws)(uint32_t count);
} mb_draw_counter_link;

// Obtain through mb_engine_recompile::draw from builtin.engine-recompile.
// Registration is available during mod_start, as with observe_records.
typedef struct mb_engine_recompile_draw {
    uint32_t struct_size;
    // The predicate may cache private metadata. It must leave GL and game
    // state alone. Zero means both callbacks for this record can be omitted.
    int (*observe_when)(const mb_record_observer* observer, mb_record_needs_draw_fn needs_draw);
    int (*register_counter_link)(const mb_draw_counter_link* link);
} mb_engine_recompile_draw;

#ifdef __cplusplus
}

#include <stddef.h>

inline const mb_engine_recompile_draw* engine_draw_api(const mb_engine_recompile* engine) {
    if (!engine || engine->struct_size < sizeof(mb_engine_recompile)) return nullptr;
    const auto* draw = engine->draw;
    return draw && draw->struct_size >= sizeof(mb_engine_recompile_draw) ? draw : nullptr;
}

inline bool observe_engine_records(const mb_engine_recompile* engine,
                                   const mb_record_observer* observer, mb_record_needs_draw_fn needs_draw) {
    if (const auto* draw = engine_draw_api(engine)) return draw->observe_when(observer, needs_draw) != 0;
    return engine && engine->struct_size >= offsetof(mb_engine_recompile, draw) &&
        engine->observe_records(observer) != 0;
}
#endif
