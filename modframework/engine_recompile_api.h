#pragma once

// Optimized Engine Remake (builtin.engine-recompile): services of its native
// engine code for other mods.
//
// Import it from a packaged mod's manifest:
//   "imports": [{ "mod": "builtin.engine-recompile", "symbols": ["mb_engine_recompile_api"] }]
// and resolve it in mod_start (NULL while the built-in is off):
//   const mb_engine_recompile* engine = (const mb_engine_recompile*)
//       api->resolve_mod_symbol(context, "builtin.engine-recompile", "mb_engine_recompile_api");
// Check struct_size before using a member added after the first version.
//
// uint32_t object addresses use the game's four-byte layout. Explicit
// pointer arguments are native addresses, as used by mod callbacks.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One render record the native record draw (process_render_record_main) is
// about to draw or just drew.
typedef struct mb_record_draw {
    uint32_t struct_size;
    uint32_t context;          // the render context the record belongs to
    uint32_t record;           // the render record; +0x830 its per-draw data
    int32_t index;             // its index in the context's record list
    uint32_t material;         // the record's material
    uint32_t effect;           // the effect: +0x48 technique, +0x4c pass, +0x50 GL program
    uint32_t program;          // the GL program the draw uses (bound)
    int32_t material_changed;  // nonzero: this record applied its material's state (the stock
                               // set_shader_render_state_for_hlsl call)
    uint32_t variant;          // the draw's last argument: the pass variant (nonzero: shadow)
    uint32_t material_flags;   // the material's flags (+0xa4): bits 8-10 its blend mode
    uint32_t colour;           // the record's mesh colour, ARGB (vMaterialColor)
    float origin[3];           // its world matrix's translation (get_world_matrix)
} mb_record_draw;

// Called on the render thread, the game's GL context current. before_draw
// runs once the record's state and uniforms are set, right before its
// glDrawElements; after_draw right after it. Either may be NULL. An observer
// may set uniforms of the bound program and state the engine sets anew per
// draw, and restores in after_draw what else it changed. It leaves the
// program, buffer bindings and vertex attributes as it found them.
// Observers nest: before_draw runs by increasing order (equal orders in the
// order they were added), after_draw the other way round. An observer that
// keeps GL state across draws which others save and restore around a draw
// (Temporal AA's colour mask) takes a lower order, so it sets the state first.
typedef struct mb_record_observer {
    uint32_t struct_size;
    void (*before_draw)(const mb_record_draw* draw);
    void (*after_draw)(const mb_record_draw* draw);
    int32_t order;
} mb_record_observer;

struct mb_engine_recompile_draw;

typedef struct mb_engine_recompile {
    uint32_t struct_size;
    // From mod_start only; the observer is copied. Returns nonzero when the
    // native record draw will call the observer for every record it draws:
    // the mod then needs no hook of its own on that function or its callees,
    // and must not link one (a hook on a callee keeps the whole draw stock).
    // Zero: not registered, the stock draw runs; hook it yourself. While a mod
    // links a hook on the record draw later (Debug Overlay's material picker),
    // the stock draw runs and observers are not called.
    int (*observe_records)(const mb_record_observer* observer);
    // Any thread. Nonzero while the native record draw runs (and calls
    // observers): no later link on process_render_record_main.
    int (*native_record_draw)(void);
    // Any thread. Nonzero while this mod runs the game function at `offset`
    // (in libMBExpMobile.so) natively: its port is linked and no mod has
    // linked a hook behind it since. Zero for functions it has no port of.
    int (*runs_natively)(uint32_t offset);
    // Render thread. A record's world frame as get_world_matrix (0x1f12ac)
    // computes it: rows x, y, z axis and origin, four floats each.
    int (*record_world_matrix)(uint32_t context, uint32_t record, float frame[16]);
    // Game thread. The scene's line of sight from `from` to `to`, at most
    // `range` long, as Agent::agent_is_visible queries it (0x5adac): nonzero
    // when nothing blocks it. Other mods' hooks on that query are not called.
    int (*line_of_sight)(uint32_t scene, const float from[3], const float to[3], float range);
    // Game thread. rglScene::get_ground_height_at (0x57fc0): the ground's
    // height under x, y; -1000000 where there is none. Other mods' hooks on
    // it are not called.
    float (*ground_height)(uint32_t scene, float x, float y);
    // Game thread. Refreshes the animated head pose and writes the eye's
    // world-space Z. Zero means call the stock get_ai_eye instead.
    int (*agent_eye_z)(const void* agent, float* z);
    // From mod_start only. Registers one callback after each completed
    // particle tick, including the direct path from native scene code.
    // Returns zero when unavailable or already occupied; use a CHAIN hook then.
    int (*observe_particle_ticks)(void (*after_tick)(const void* system));
    // Optional draw integration (engine_recompile_draw_api.h).
    const struct mb_engine_recompile_draw* draw;
} mb_engine_recompile;

#ifdef __cplusplus
}
#endif
