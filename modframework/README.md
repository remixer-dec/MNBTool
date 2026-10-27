# M&BTool ModFramework

Native mod SDK for the Mount & Blade: Warband launcher. One source package can
build for both the original ARMv7 launcher and the translated ARM64 launcher.
Copy this entire directory: it contains the headers, build script and compiler
producer. No ModFramework source checkout or PORT_ENGINE installation is needed.

| File | What it is |
| --- | --- |
| `mod_api.h` | The mod interface, ABI v1: everything a mod calls goes through it. |
| `build-mod.sh` | Builds ARMv7, ARM64, or both, generates game-hook bindings, and checks each object against the loader rules. |
| `mod_compat.hpp` | Public typed helpers used by compiler-generated game-hook bindings. |
| `generate-bindings.py`, `_compiler/` | Standalone compiler producer, shared with PORT_ENGINE; requires standard Python 3.9+. |
| `demo-mod/` | A small working mod (frame-rate log) that shows the expected style, templates included. |

## Quick start

```sh
export ANDROID_NDK_ROOT=/path/to/android-ndk
cp -R demo-mod my-mod
# Edit my-mod/manifest.json (id, name, author) and my-mod/mod.cpp.
ANDROID_ABI=both ./build-mod.sh my-mod my-mod/out
```

The output folder is the package: `manifest.json`, `mod.armv7.o` and
`mod.arm64.o`. Import the **whole folder** through the launcher's mod manager;
it selects the object matching the launcher ABI. A new package starts disabled.
Changes apply on the next game launch. Both objects must pass before a dual-ABI
build publishes its output; a rejected rebuild preserves the existing package.

For a single target, use `ANDROID_ABI=armeabi-v7a` (the default) or
`ANDROID_ABI=arm64-v8a`. ARMv7 still works with the original GCC toolchains.
ARM64 automatic compatibility requires NDK Clang and Python 3.9 or later.

### Automatic game hooks

Write ordinary `api->hook_function(context, &request)` calls. The ARM64 build
first compiles the original source for ARMv7 to derive its function signatures,
then generates public typed bindings. It keeps the source unchanged and routes
the request through the framework's compatible hook service, preserving chain,
replacement and conflict policy. No callback signature list or per-mod adapter
is required. EGL/GL import hooks and platform calls keep their native ABI.

The current producer supports C++17, constant library/offset targets, typed
replacement functions and matching typed original slots. Requests may use an
aggregate initializer or fields assigned once before the call. Scalar signatures
include signed/unsigned integers, explicit 64-bit integers, floats, doubles and
scalar memory pointers; the ARMv7 contract supplies argument alignment and stack
words. Use fixed-width types for game values and game storage. Platform-owned
structures such as the argument to `clock_gettime` remain native.

This example belongs in a mod whose manifest declares the matching game build
and hook. Replace the example offset with the offset verified for that build:

```cpp
#include "mod_api.h"
namespace my_mod {
using Function = int32_t (*)(int32_t, double);
Function previous = nullptr;
int32_t replacement(int32_t value, double scale) {
    return previous(value, scale); // add your behavior here
}
mf_result_v1 install(const mf_api_v1* api, const mf_mod_context_v1* context) {
    const mf_function_hook_v1 hook{sizeof(hook), MF_HOOK_CHAIN, "my-hook",
        "libMBExpMobile.so", nullptr, 0x1000,
        reinterpret_cast<void*>(&replacement), reinterpret_cast<void**>(&previous)};
    return api->hook_function(context, &hook);
}
}
```

Unsupported hook contracts fail during the build with a reason code. Record-pointer
signatures, pointer arrays, resource handles, variadic/function-pointer arguments, dynamic
targets and escaped hook services need additional generated contracts; they
are not silently passed to the ARM64 game as raw native hooks. C11 packages
without game hooks remain supported. Byte patches still require the launcher's
source-qualified patch support; generating a hook binding does not qualify a
patch or prove that a mod renders correctly in the game.
Memory pointers are for scalar storage with the declared source width. Casting
them to native records does not provide an ARMv7 game-layout view; those views
remain a separate compatibility requirement.

### Game addresses

Game structures store addresses as 32-bit words. Read them as `uint32_t` and convert
them to a pointer as usual:

```cpp
uint32_t address;  // the game string's text pointer, at +4
memcpy(&address, static_cast<const char*>(string) + 4, sizeof address);
const char* text = reinterpret_cast<const char*>(static_cast<uintptr_t>(address));
```

On ARM64 the game's memory is not at its game addresses, so the build routes every
conversion of a 32-bit integer (`uint32_t`, `int32_t`, `unsigned`, `int`, also through a
widening cast) to a pointer through the framework (`mf_game_pointer_v1`, generated calls).
Values that are or contain `uintptr_t`/`size_t` (native addresses) and literal constants stay
as written. Conversions are found in the mod source and in the headers in its folder and
below. The build rejects the rest with a reason code:

| Reason | Do instead |
| --- | --- |
| `PUBLIC_GAME_POINTER_CODE` | A game function address cannot be called natively; call game functions through `resolve_game_symbol`/`resolve_game_address`, or hook them. |
| `PUBLIC_GAME_POINTER_MACRO` | Convert game addresses in a function, not in a macro. |
| `PUBLIC_GAME_POINTER_HEADER` | Keep files that convert game addresses in the mod source's folder or below it. |

### Calling game functions

Call game functions as usual: convert the result of `resolve_game_symbol`, or of
`resolve_game_address` with `MF_MEMORY_EXECUTE`, straight to a function pointer and
call it, in any source or header file of the package:

```cpp
using GetHandle = uint32_t (*)(const void* texture);
auto get_handle = reinterpret_cast<GetHandle>(
    api->resolve_game_symbol(context, "libMBExpMobile.so", "_ZNK10rglTexture15get_cur_handle0Ev"));
```

The ARM64 build records each prototype from the ARMv7 graph (`mf_game_functions_v1`, generated;
authors never write it) and the framework returns a native adapter for it, so the same source
runs on both games. Targets must be constant, and one target keeps one prototype. Data symbols
need nothing: `resolve_game_symbol` returns their game memory on both. A function address kept
as `void*` and converted later gets no adapter (NULL on ARM64).

Arguments and results are integers up to 64 bits, `bool`, `float`, `double` and pointers.
Pointer arguments must point at memory the game can see: game memory, or the package's
globals, heap or local variables (ARM64 builds keep address-taken locals on a per-thread
stack inside the game's memory, see below). The build rejects the rest with a reason code
and what to declare instead:

| Reason | Declare instead |
| --- | --- |
| `PUBLIC_GAME_CALL_VARIADIC` | Only the fixed arguments you pass; format text in the mod and pass `"%s"` with it. |
| `PUBLIC_GAME_CALL_CALLBACK` | Game code cannot call native mod functions; hook the game function that calls it. |
| `PUBLIC_GAME_CALL_REFERENCE` | A pointer (`const T*` for `const T&`) to game-visible memory. |
| `PUBLIC_GAME_CALL_RECORD` | A struct as a pointer, or, if every field is 4 bytes (e.g. `float x, y`), its fields in order; an enum as `int`. |
| `PUBLIC_GAME_CALL_RECORD_RESULT` | A pointer to a static or heap result buffer as the first argument, returning `void`; or `int` for results of 4 bytes or less. |
| `PUBLIC_GAME_CALL_LONG_DOUBLE` | `double` (the game's `long double` is a `double`). |
| `PUBLIC_GAME_CALL_TYPE` | A fixed-width integer, `float`, `double` or pointer type. |
| `PUBLIC_GAME_CALL_STATIC_TARGET_REQUIRED` | A string literal (or constant) library and symbol, a constant offset. |
| `PUBLIC_GAME_CALL_CONFLICTING_PROTOTYPES` | One prototype per game function. |

Authors maintaining an explicit native ARM64 implementation may opt out with
`MF_COMPAT_MODE=native ANDROID_ABI=arm64-v8a ./build-mod.sh my-mod` and use
`mod_compat.hpp` themselves. This opt-in does not bypass runtime admission.

## Toolchain

Any compiler that targets 32-bit ARM works. `build-mod.sh` uses the first it finds,
unless you set one; ARM64 and dual-target builds use NDK Clang:

| Variable | Default | Notes |
| --- | --- | --- |
| `CXX` | Android NDK clang (`ANDROID_NDK_ROOT`), `clang++`, `arm-linux-gnueabi-g++`, `arm-none-eabi-g++` | Clang gets `--target=armv7a-linux-androideabi24`, GCC `-march=armv7-a -mfpu=neon`; both `-mfloat-abi=softfp`. |
| `OBJCOPY` | `llvm-objcopy`, or the GCC toolchain's `objcopy` | Must read ARM ELF files. |
| `READELF` | `llvm-readelf`, the GCC toolchain's `readelf`, `readelf` | |
| `CXXFLAGS` | | Extra flags, e.g. `-Werror` or `-I<dir>`. |
| `ANDROID_ABI` | `armeabi-v7a` | `armeabi-v7a`, `arm64-v8a`, or `both`. |
| `PYTHON` | `python3` | Standard Python 3.9+ for automatic ARM64 bindings. |
| `MF_COMPAT_MODE` | `automatic` | `native` opts into an author-managed ARM64 implementation. |

Tested with Android NDK clang. Plain LLVM clang and GCC ARM cross compilers are
supported by the script but not tested.

The script compiles one file (`mod.cpp` or `mod.c`, also accepted under `native/`).
ARMv7 uses ARM mode. Both targets use no PIC, no exceptions, RTTI, unwind tables or stack protector, and
freestanding headers. ARM64 builds use Clang's safe stack (`-fsanitize=safe-stack -mllvm
-safestack-use-pointer-address`): the launcher provides `__safestack_pointer_address` and keeps
the thread's stack pointer on its own stack, which Android and the system libraries require. It then strips the ARM unwind index and refuses an object the
launcher would refuse:

| Refused | Usual cause and fix |
| --- | --- |
| not a 32-bit little-endian ARM `ET_REL` object, EABI version 5 | wrong compiler or target; a linked file (compile with `-c` only) |
| not a 64-bit little-endian AArch64 `ET_REL` object (ARM64 target) | wrong compiler/target, linked file or unsupported ELF flags |
| hard-float object | `armhf`/`gnueabihf` defaults: use `-mfloat-abi=softfp` |
| Thumb relocations (`R_ARM_THM_*`) | `-mthumb`: use `-marm` |
| GOT/PIC relocations | use `-fno-pic` |
| sections other than code, data and zero-filled data | static constructors (`.init_array`), unwind index, thread-locals |
| imports outside the list below | see [What a mod may call](#what-a-mod-may-call) |
| no `mod_start` | missing `MF_MOD_START` |

Allowed relocations: `R_ARM_ABS32`, `REL32`, `CALL`, `JUMP24`, `MOVW_ABS_NC`,
`MOVT_ABS`, `TARGET1`, `V4BX`, `NONE`.
ARM64 allows `R_AARCH64_ABS64`, `ABS32`, `PREL32`, `CALL26`, `JUMP26`,
`ADR_PREL_PG_HI21`, `ADR_PREL_LO21`, `ADD_ABS_LO12_NC`,
`LDST8/16/32/64/128_ABS_LO12_NC`, and `NONE`.

## What a mod may call

A mod object may import only these functions; everything else goes through
`mf_api_v1`:

- `memcmp memcpy memmove memset strlen strnlen strcmp strncmp strstr`
- `malloc calloc realloc free clock_gettime`
- ARM helpers the compiler emits: `__aeabi_memcpy/memmove/memset/memclr` (plain, `4`
  and `8` variants), `__aeabi_uidiv idiv uidivmod idivmod`, `__aeabi_lmul`

The compiler adds helper calls of its own. Two common ones are **not** available:
64-bit division or modulo (`__aeabi_ldivmod`, `__aeabi_uldivmod`), and conversions
between floating point and 64-bit integers. Keep that arithmetic in 32 bits.

No other C library function is available, `snprintf` included: text with values is
written as a template ([Templates and text edits](#templates-and-text-edits)). Declare
the allowed functions yourself, since most libc headers do not build freestanding (see
the demo's `clock_gettime`).

## Manifest

The demo's manifest is a complete minimal example. Required fields:
`schemaVersion` (1), `id`, `name`, `version`, `modAbi` (1).

| Field | Notes |
| --- | --- |
| `id` | `[a-z0-9][a-z0-9._-]*`, at most 128 characters; `builtin.` is reserved. |
| `author`, `summary`, `category` | Shown in the manager. |
| `localizations` | `{ "es": { "name": ..., "summary": ... } }`; missing entries fall back to English. |
| `hooks` | Every hook and patch the mod will request (see [Changing the game](#changing-the-game-hooks-and-patches)). |
| `settings` | Values the player edits in the manager (below). |
| `gameBuilds` | The game builds the mod runs on; required for offsets, byte patches and raw addresses (see [Game builds](#game-builds)). |
| `exports` / `imports` | Symbols shared with other mods (see [Working with other mods](#working-with-other-mods)). |
| `compatibility` | `requires` (`[{ "id": ..., "version": ... }]`), `compatible`, `incompatible`, `loadAfter`, `loadBefore`, `supersedes` (lists of ids). |

### Settings

```json
"settings": { "fields": [
  { "key": "interval", "type": "INT", "label": "Interval (s)", "default": 5, "min": 1, "max": 60 }
] }
```

| Type | Read with | Extra fields |
| --- | --- | --- |
| `BOOL` | `get_setting_bool` → `int` (0 or 1) | |
| `INT` | `get_setting_int` → `int32_t` | `min`, `max`, `step` |
| `FLOAT` | `get_setting_float` → `float` | `min`, `max`, `step` |
| `STRING` | `get_setting_string` → `const char*` | `maxLength` (default 256), `options: [{ "value", "label" }]` |

Keys are lowercase words separated by `.`, `_` or `-` (at most 64 characters).
`api->get_setting_int(context, "interval", 5)` returns the player's value or the
manifest default, already within `min` and `max`. The last argument is only a fallback
for a key that is undeclared or declared with another type; the launcher logs that
mistake. Values are fixed for the game process.

### Game builds

Offsets only make sense for one exact build of the game library, so a mod that uses
offsets, byte patches or raw addresses must list the builds it was made for. Each
entry is either:
- `"default"`: every build this launcher recognizes. Use it when you made the mod
  against the game the launcher supports.
- `"sha256:<hex>"`: one specific build, the SHA-256 of its `libMBExpMobile.so`.

On any other build the manager refuses to enable the mod. A mod without offsets can
leave `gameBuilds` out and runs on any build.

## Changing the game: hooks and patches

A mod never writes to game code itself. It asks the framework, through a **request**,
to install each hook or patch. The framework then:
- checks the request against the manifest, which the player sees in the manager and
  can switch off hook by hook;
- settles which mod gets what when several mods touch the same place;
- installs everything safely, and connects chained hooks.

Use a request in `mod_start`, once for each hook or patch declared in the manifest.
There are three kinds:

| You want to | Function | Request struct | Declaration `mechanism` |
| --- | --- | --- | --- |
| Intercept a call the game makes to a system library, e.g. `eglSwapBuffers` | `hook_import` | `mf_import_hook_v1` | `BYTEHOOK_IMPORT` |
| Wrap or replace one of the game's own functions | `hook_function` | `mf_function_hook_v1` | `SHADOWHOOK_UNIQUE` |
| Change a few bytes of game code or data in place | `patch_bytes` | `mf_byte_patch_v1` | `PATCH` |

The manifest declares each hook, and the request repeats the same values at run time,
adding your function:

```json
{ "id": "frame-end", "mechanism": "BYTEHOOK_IMPORT", "behavior": "CHAIN",
  "caller": "libMBExpMobile.so", "target": "libEGL.so", "symbol": "eglSwapBuffers" }
```

```cpp
mf_import_hook_v1 request{};                 // start from all zeroes
request.struct_size = sizeof(request);       // tells the launcher which struct version this is
request.behavior = MF_HOOK_CHAIN;            // the same values as the declaration...
request.hook_id = "frame-end";
request.caller_library = "libMBExpMobile.so";
request.callee_library = "libEGL.so";
request.symbol = "eglSwapBuffers";
request.replacement = (void*)&my_swap;       // ...plus your function
request.previous = (void**)&g_previous_swap; // where the framework stores what to call next
mf_result_v1 result = api->hook_import(context, &request);
```

A request that does not match a declaration, or whose hook the player switched off,
is refused with `MF_ERROR_DENIED`. The other two requests work the same way:
- **Function hooks** (`mf_function_hook_v1`) name `target_library` and either an
  `offset` (needs [game builds](#game-builds)) or an exported `symbol`. The address must
  be the first instruction of a function. `original` receives the function to call
  through.
- **Byte patches** (`mf_byte_patch_v1`) give `offset`, `byte_count`, the
  `expected_bytes` now there, and the `replacement_bytes`. If the current bytes differ,
  nothing is written (`MF_ERROR_PATCH_MISMATCH`).

Import hooks: `caller` must be a game library (`libMBExpMobile.so`, `libfmodex.so`).
System functions are limited to graphics (`gl*`, `egl*`), logging, input and window
queries, maths, memory/string, time and locking.

`behavior` says how you share the target with other mods:
- `CHAIN`: your replacement always calls the previous function (`previous` or
  `original`), so other mods' `CHAIN` hooks on the same target run too. Read the
  pointer at every call: it changes when another mod joins the chain.
- `REPLACE`: you may skip the original. A second claim on the same target conflicts.
  Built-in mods' links come first and chain too, except on the few functions a built-in
  implements itself; the launcher names the built-in when a plan conflicts.
- `EXCLUSIVE`: nobody else may claim the target.

## Working with other mods

- **Depend on a mod:** list it in `compatibility.requires`. Your mod can then be
  enabled only together with it, and starts after it.
- **Adapt to a mod without depending on it:** call `is_mod_active(context, "other.id")`
  in `mod_start`, and `get_mod_version` when its versions behave differently.
  Typical uses: skip your fix when another mod already provides it, or switch to a
  path that cooperates with it. The answer covers the whole launch, including mods
  that start after yours.
- **Use another mod's functions or data:** the provider lists the global symbols it
  shares in `"exports": ["name"]`. You declare `"imports": [{ "mod": "<id>", "symbols":
  ["name"] }]` and get the address with `resolve_mod_symbol`. Providers start before
  their users.

## Templates and text edits

Shader code, or any other text, is written as a template that the launcher renders,
instead of being joined from pieces in C. Values are typed, so a number always comes
out as valid GLSL.

```cpp
constexpr char kTint[] = R"glsl(
uniform vec4 mb_tint;
{% if strong %}
const float kAmount = {{ amount }};
{% endif %}
)glsl";

const mf_template_value_v1 values[] = {MF_BOOL("strong", strong), MF_FLOAT("amount", 0.8f)};
char* text = api->render_template(context, kTint, values, MF_COUNT(values));  // free() it
```

| Syntax | Result |
| --- | --- |
| `{{ name }}` | The value: `true`/`false`, an integer, a float as a GLSL literal (`4.0`, `0.0025`), a string as it is. Negative numbers come in parentheses: `(-0.5)`. |
| `{% if name %}` … `{% elif name %}` … `{% else %}` … `{% endif %}` | The branch whose `BOOL` value is true; `if not name` negates. |
| `{% for item in list %}` … `{% endfor %}` | Repeats for each item of a `LIST` value (`MF_LIST(name, items, count)`: items of one type, their names ignored). |
| `{% for i in count %}` … `{% endfor %}` | Repeats with `i` = 0 … count − 1; `count` is an `INT` value or a number. |

A line holding only a `{% %}` tag disappears entirely. Mistakes are errors, logged with
their line: an unknown name (in any branch or loop, taken or not), a reused name, a
condition that is not a `BOOL`, an unclosed tag, more than 1024 repeats. `{{` and `{%`
always open a tag. What only the driver knows stays GLSL, e.g.
`#ifdef GL_FRAGMENT_PRECISION_HIGH`.

**Editing the game's text.** `edit_text` applies a list of edits to a copy of a text,
such as a game shader as it loads:

```cpp
constexpr mf_text_edit_v1 kEdits[] = {
    {MF_EDIT_REQUIRE, "tmpvar_1.xyz = inPosition;", nullptr},  // the shader we know
    {MF_EDIT_BEFORE, "void main ()", kHelpers},                 // a template
    {MF_EDIT_REPLACE, "gl_FragColor = {{ colour }};", "gl_FragColor = mb_out ({{ colour }});"},
};
char* edited = api->edit_text(context, source, kEdits, MF_COUNT(kEdits), values, MF_COUNT(values));
// NULL: not the shader these edits were written for; it stays as it was.
```

| Op | Does |
| --- | --- |
| `MF_EDIT_REQUIRE` | The anchor must be present; no text. |
| `MF_EDIT_BEFORE`, `MF_EDIT_AFTER` | Inserts the text before or after the anchor. |
| `MF_EDIT_REPLACE` | Replaces the anchor. |
| `MF_EDIT_REPLACE_ALL` | Replaces every occurrence. |
| `MF_EDIT_PREPEND`, `MF_EDIT_APPEND` | Adds the text at the start or the end; no anchor. |

- Edits apply in order, each to the previous one's result, and all or nothing.
- `BEFORE`, `AFTER` and `REPLACE` need their anchor exactly once, `REPLACE_ALL` at least
  once. A missing or repeated anchor means the text is not the one the edits were
  written for: `edit_text` returns NULL and nothing changes. `| MF_EDIT_OPTIONAL` skips
  an edit whose anchor is missing instead.
- `{{ name }}` in an anchor captures text: the shortest run, with balanced brackets and
  no `;` outside them, that the anchor's next literal text follows. The edit's
  template reads it as a string; above, `colour` is the whole expression.
- The texts are templates using the call's values. NULL also means no edit changed
  anything; a mistake in the edits is logged.

**Game shaders.** The game builds each program from GLSL ES 1.00 files, vertex shader
first, and hands each to `glShaderSource` as one NUL-terminated string. An import hook
on it (`caller` `libMBExpMobile.so`, `target` `libGLESv2.so`) sees every shader and
needs no game builds:
- Pass an edited shader on as two strings, `{edited, ""}`, and `free()` it after the
  call. While the game builds its shaders, the launcher frees a lone string once GL has
  copied it (the game would leak it otherwise), and yours must not be freed twice.
- Other mods may have edited the text before you see it, e.g. renamed `main`. Anchor
  on what your edit needs, and leave the shader alone when `edit_text` returns NULL.

## Writing a mod

- **Entry point.** `MF_MOD_START(name)(const mf_api_v1* api, const mf_mod_context_v1*
  context)` exports `mod_start`. It runs once per game launch, after the game library
  has loaded and before the game starts. Mods stay loaded for the whole process
  (`mod_stop` is reserved and never called). Return `MF_OK`, or an error to report
  that the mod failed.
- **Check the API.** Check `abi_version` and use `MF_API_HAS` for the functions you
  need (see [Compatibility](#compatibility)).
- **Static state only.** Use globals that are zero or constant initialised. There are no
  global constructors, exceptions, RTTI or thread-locals. Keep `previous`/`original`
  slots and patch byte arrays in static storage: the framework writes to them later.
- **Threads.** A hook runs on the thread that called the hooked function. Call GL only
  on the game's render thread, e.g. inside its `eglSwapBuffers` (end of frame).
- **Logging.** `api->log` writes to logcat under the tag `ModFramework` as
  `[<mod id>] <message>`, at most 2048 bytes. Log activation and failures, not every
  frame.
- **Text.** Shader code and messages with values are templates
  ([Templates and text edits](#templates-and-text-edits)), never pieces joined by hand.
  Keep them as `R"glsl(...)glsl"` raw strings next to the code that uses them.
- **Exact signatures.** A replacement must match the target's ABI exactly. On
  armeabi-v7a, `float` arguments travel in core registers (soft-float calling
  convention).

## Compatibility

- **ABI version.** The header, the API table (`api->abi_version`) and the manifest
  (`modAbi`) all say 1. A change that breaks existing mods would raise the version, and
  a launcher loads only mods built for its own ABI version.
- **The API table only grows.** Within ABI v1, new functions are added at the end of
  `mf_api_v1`; nothing is removed or moved. `api->struct_size` is the size of the table
  the running launcher provides. So:
  - A mod built with an older header works on newer launchers.
  - A mod built with a newer header may run on an older launcher that lacks the
    newest functions. Check each one you use with `MF_API_HAS(api, function)` before
    calling it: run with fewer features if it is optional, or return an error if the
    mod cannot work without it. Checking the newest function you need covers every
    one before it (the demo checks `render_template`).
- **Requests are fixed.** The request structs do not change within ABI v1, and
  `struct_size` must equal their v1 size.
- **Manifests.** Launchers ignore manifest fields they do not know, so a newer
  optional field is simply unused on an older launcher.

## API reference (`mf_api_v1`)

| Function | Does | Returns |
| --- | --- | --- |
| `is_mod_active(ctx, id)` | Whether another mod runs in this launch ([Working with other mods](#working-with-other-mods)). | 1 or 0 |
| `get_mod_version(ctx, id)` | That mod's version string. | text or NULL |
| `log(ctx, priority, message)` | Writes a log line (`MF_LOG_DEBUG/INFO/WARNING/ERROR`). | |
| `hook_import(ctx, request)` | Installs an import hook ([Changing the game](#changing-the-game-hooks-and-patches)). | `mf_result_v1` |
| `hook_function(ctx, request)` | Installs a function hook. | `mf_result_v1` |
| `patch_bytes(ctx, request)` | Applies a byte patch. | `mf_result_v1` |
| `resolve_game_symbol(ctx, library, symbol)` | Address of an exported symbol in a game library; for a function, callable as described in Calling game functions. | pointer or NULL |
| `resolve_game_address(ctx, library, offset, bytes, access)` | Checked address of game memory at an offset; `access` combines `MF_MEMORY_READ/WRITE/EXECUTE`. Needs `gameBuilds`. | pointer or NULL |
| `storage_open(ctx, scope, path, flags)` | Opens a file (Storage below). | descriptor, or negative |
| `storage_read/write(ctx, fd, buffer, bytes)` | Reads or writes. | byte count, or negative |
| `storage_seek(ctx, fd, offset, whence)` | Moves the file position (`whence`: 0 set, 1 current, 2 end). | new position, or negative |
| `storage_close(ctx, fd)` | Closes a descriptor from `storage_open`. | `mf_result_v1` |
| `post_java_message(ctx, data, bytes)` | Sends bytes to the package's Java component (not covered here). | `mf_result_v1` |
| `get_launcher_config(ctx, key)` | A value the launcher publishes (keys below). | text or NULL |
| `resolve_mod_symbol(ctx, mod_id, symbol)` | A symbol another mod exports. | pointer or NULL |
| `resolve_gl_function(ctx, name)` | Any GL ES or EGL function (`gl…`/`egl…` + uppercase letter), core or extension. The game's context decides what may be called: check `GL_VERSION`/`GL_EXTENSIONS` before using anything beyond ES 2.0. | pointer or NULL |
| `get_setting_bool/int/float/string(ctx, key, fallback)` | This mod's setting, typed as declared ([Settings](#settings)). | the value, or `fallback` |
| `render_template(ctx, text, values, count)` | Renders a template ([Templates and text edits](#templates-and-text-edits)); release the result with `free()`. | text or NULL |
| `edit_text(ctx, source, edits, count, values, value_count)` | Edits a copy of a text; release the result with `free()`. | text or NULL |
| `vibrate(ctx, milliseconds, strength)` | Vibrates the device (up to `MF_VIBRATION_MAX_MILLISECONDS`, strength 1-255); a call replaces a running vibration. Check `MF_API_HAS(api, vibrate)`. | `mf_result_v1` |
| `read_motion(ctx, motion)` | The newest gyroscope (rad/s) and accelerometer (m/s², gravity included) readings in the device's axes (`mf_motion_v1`, `sensors` says which are present). The first call starts the sensors. Check `MF_API_HAS(api, read_motion)`. | `mf_result_v1` |

`get_setting_text` is superseded by the typed getters and stays in the table only for
mods built before them.

**Results (`mf_result_v1`).**

| Result | Meaning |
| --- | --- |
| `MF_OK` | Done. |
| `MF_ERROR_INVALID_ARGUMENT` | Malformed request. |
| `MF_ERROR_DENIED` | Not declared, switched off in the manager, or not allowed (e.g. an OS import outside the permitted groups, or not a function's first instruction). |
| `MF_ERROR_CONFLICT` | Another mod holds the target. |
| `MF_ERROR_NOT_FOUND` | No such library, symbol or import. |
| `MF_ERROR_HOOK_FAILED` | The hook could not be installed. |
| `MF_ERROR_PATCH_MISMATCH` | The original bytes differ; nothing was written. |
| `MF_ERROR_IO` | A file operation failed. |

**Context (`mf_mod_context_v1`).** `mod_id`, `mod_version` and `game_build_id`
describe this mod and launch. Pass the context back unchanged to every call.
`reserved` and `runtime_handle` are framework-owned.

**Launcher config keys.**
- `MF_LAUNCHER_CONFIG_GAME_MODULE_DIRECTORY`: the selected module's folder name.
- `MF_LAUNCHER_CONFIG_GAME_LANGUAGE`: the game language code.
- `MF_LAUNCHER_CONFIG_DISPLAY_HDR`: the screen's HDR types (`hdr10 hlg hdr10+
  dolby-vision`, space-separated; empty for SDR).

**Storage.**
- `MF_STORAGE_MOD` opens files in the mod's private folder. `MF_STORAGE_GAME` opens
  files beneath the player's chosen game folder (`/sdcard/mnb` or the app-data
  game folder), including game resources in `main/`, `patch/`, module directories,
  and downloaded multiplayer maps in `SceneObj/`. Use game scope for these files,
  rather than paths escaping the mod's private folder. Open `SceneObj/<name>.sco`
  directly in game scope; the private SceneObj link is only for engine access.
- Paths are relative. `.`, `..`, absolute paths and symlinks are refused.
- `flags`: `O_RDONLY` 0, `O_WRONLY` 1 or `O_RDWR` 2, combined with `O_CREAT` 0x40,
  `O_TRUNC` 0x200, `O_APPEND` 0x400. New files are created `0600`.


For dual-architecture source, automatic ARM64 translation defines
`MF_TRANSLATED_ARM64=1` during both ARM32 ABI analysis and ARM64 compilation.
Use `#if defined(MF_TRANSLATED_ARM64)` for translated-only sections, or
`#if !defined(MF_TRANSLATED_ARM64)` for ARMv7-only code. Native ARMv7 builds
leave it undefined. `__aarch64__` alone selects different branches during ABI
analysis and native compilation and cannot replace this translation guard.
