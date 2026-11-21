#pragma once

// Essential Controls: the shared input layer other mods drive and extend.
//
// Import it from a packaged mod's manifest:
//   "imports": [{ "mod": "builtin.controls", "symbols": ["mb_controls_api"] }]
// and resolve it in mod_start:
//   const mb_controls* controls = (const mb_controls*)
//       api->resolve_mod_symbol(context, "builtin.controls", "mb_controls_api");
//
// Model: inputs --(bindings)--> actions --(targets)--> the game.
//
// * An input is any named source of a value: "key:96" is the Android keycode
//   96 (gamepad A) from any device; mods invent their own ("touch.jump",
//   "gyro"). A pressed button is (1, 0), released (0, 0); axes pass (x, y).
//   Pad triggers and D-pads that report axes arrive as their key inputs too:
//   key:104 / key:105 (left / right trigger), key:19..22 (up, down, left, right).
//   The pad's sticks are axis:left and axis:right (x right, y up); while one
//   is bound, the game's own reading of that stick is replaced.
//   Mice: mouse:left, mouse:right, mouse:middle, mouse:back, mouse:forward
//   (the BACK and FORWARD keys Android also sends for the last two are dropped);
//   mouse:wheel_up / mouse:wheel_down (a short press per notch); mouse:move
//   (pixels moved since the last frame, x right, y up; captured or not).
// * An action is a named thing to do, available only while its condition
//   holds. Its targets are space-separated:
//     key:<slot>             hold the game's key slot (an Android keycode)
//     hold:<slot>            the same, without a new press or a release: code
//                            that waits for an edge ignores it (slot 0, the
//                            port's slot for PC keys Android lacks, is also
//                            the console key and Escape; Ctrl in the game's
//                            code, as Shift is slot 59)
//     control:<n>            hold whatever key the game's control n (0..51)
//                            is bound to (move, kick, quick save...)
//     stick:left|right       set that stick to the action's (x, y)
//     stick:left:<x>:<y>     push that stick by (x, y) scaled by the value
//     look                   turn the battle camera by (x, y) pixels (y up),
//                            as a PC mouse does; also steers attack direction
//   Targets split by "then" are steps: a press of the action holds each step
//   for two frames and lets go for two, in turn, even if released meanwhile
//   ("control:29 then control:23 then control:25" orders everyone to charge).
//   An action without targets takes its inputs and does nothing, which
//   mutes a button. An action with a callback runs mod code instead (on the
//   game thread).
// * A condition is "-" (always) or comma-separated terms that must all hold:
//   name=value, name=v1|v2, name!=value. Contexts are named strings; the
//   layer publishes "window" (the top game window: map, tactical, menu,
//   conversation, inventory, ...), "cursor" ("hidden" while a battle shows
//   no mouse cursor, else "visible") and "mouse" (the first mouse button
//   held: left, right, middle, back, forward, or empty) and "troops" ("yes"
//   in a battle with someone to give orders to: a living person of the player's
//   team who came with a party, or any multiplayer game; else "no"); mods add
//   their own.
// * A bound input is taken from the game only while one of its actions is
//   available; otherwise the game handles it as usual.
//
// Every function is thread-safe and returns 1 on success, 0 otherwise.
// Names are case-sensitive, 1..63 characters, without spaces; conditions
// up to 255.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mb_controls_callback)(const char* action, float x, float y, void* user);
typedef const char* (*mb_controls_context_reader)(void* user);

typedef struct mb_controls {
    // Defines or replaces an action; `when` may be NULL or "-" for always.
    int (*define_action)(const char* name, const char* when, const char* targets);
    // As above, but the callback gets every change of the action's value.
    int (*define_callback_action)(const char* name, const char* when,
                                  mb_controls_callback callback, void* user);
    int (*remove_action)(const char* name);

    // Several actions may share an input: the first available one takes it.
    int (*bind)(const char* input, const char* action);
    // A NULL action removes every binding of the input.
    int (*unbind)(const char* input, const char* action);

    // Feeds an input through its bindings; 1 when an action took it.
    int (*set_input)(const char* input, float x, float y);
    // Drives an action directly, bypassing bindings; (0, 0) releases it.
    int (*set_action)(const char* action, float x, float y);
    // Whether the action exists and its condition holds this frame.
    int (*is_available)(const char* action);

    // Adds a context; `read` runs once per frame on the game thread and
    // returns a value that stays valid until its next call.
    int (*define_context)(const char* name, mb_controls_context_reader read, void* user);
    // Copies the context's current value, NUL-terminated.
    int (*get_context)(const char* name, char* value, size_t size);

    // Shapes an input's values as they arrive: a radial dead zone (0..1; the
    // rest of the range is rescaled), then a scale per axis (negative inverts).
    // A zero dead zone and scale (1, 1) remove the shape.
    int (*shape_input)(const char* input, float dead_zone, float scale_x, float scale_y);
} mb_controls;

#ifdef __cplusplus
}
#endif
