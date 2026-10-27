# Water Physics & Drowning

Standalone optional gameplay mod (`mbtool.water`). Installing it leaves it
disabled until selected in Engine Mods. It is excluded from APK bundling and
graphics presets; no launcher or frontend update is needed.

Build with the NDK and SDK from the public MNBTool repository root:

```sh
ANDROID_ABI=both ./modframework/build-mod.sh mods/water
```

In the launcher development checkout:

```sh
ANDROID_ABI=both ./public/build-mod.sh optional-mods/water
```

Zip `manifest.json`, `mod.armv7.o` and `mod.arm64.o` from the resulting `build/`
directory at the archive root, install through Engine Mods → Add more → From
device, enable the mod and restart the game. The mod has no settings: multiplayer
filters allowed mods by id and version, so every player must simulate the
same water.

## Behavior

- Human wading speed defaults to half the game's already reduced speed.
- Downward velocity in water is capped at 2 m/s before and after each physics
  step. Horizontal velocity has time-based drag, including horses and AI.
  Upward jumps keep their velocity. Gravity within the step is unchanged, so
  there can be one step of overshoot on water entry. Horse ground locomotion
  still uses the game's horse controls; the wading multiplier is human-only.
- Water landings suppress the fall blow while preserving any queued combat
  blow, including when the fall would have overwritten a weaker combat hit.
  Dry falls retain normal damage. This follows the engine's flat water-plane
  predicate, so even shallow water suppresses fall damage.
- Living humans have 10 seconds of breath. Only a fully submerged mouth uses
  it. After exhaustion, one 6-point environmental hit is applied per second,
  before the game's difficulty and mission rules. Air immediately stops damage
  and restores four seconds of breath per second. Horses do not drown.
- Single player and multiplayer hosts/servers simulate the mod. Clients leave
  movement and damage to the authoritative game. Multiplayer prediction and
  replication still need device validation.
- Breath follows simulation time, pauses on zero delta, caps a long stall to
  0.25 seconds, and resets on respawn, vacant slots and mission clear/rebuild.
  There is no oxygen HUD; the timer is internal and damage uses the game's
  normal received-damage feedback.

## Mouth detection and evidence

The engine has no mouth accessor. The mod calls `Agent::get_ai_eye` (`0x5aa70`)
to refresh the animated head even off camera, then uses head bone 9's frame
and the entity's world transform. The mouth is modeled as a box centred 10 cm
below that eye, with half-extents 4 cm sideways, 2.5 cm forward and 2 cm upward.
Its highest world-space point must be **strictly below** `Mission+0xc1b8`.
Touching the surface or exposing part of the box restores breath. Animation,
mounting, tilt and scale affect this test; missing/invalid skeleton data never
falls back to a guessed feet height. Dry agents skip pose evaluation.

This is an anatomical approximation for the standard humanoid skeleton, not
a query of the rendered lips. Face morphs and custom heads may need calibration
after in-game testing. The game represents mission water as one horizontal
plane, so the mod cannot identify custom water volumes or dry tunnels below it.

Source evidence: `/tmp/THE_GAME_RE/WATER.md`, the original engine bytes,
`touch_combat.cpp`, `touch_common.h`, `script_operations_pc.cpp`, and the optional
Fire Damage mod's verified environmental-blow layout. Two corrections to the
water research are essential:

1. `get_ideal_speed_human` returns an `rglVec2` through a hidden result pointer;
   its ABI is `void(float* result, const void* agent)`, not a float return.
2. `basic_game+0x76d4` is multiplayer mode (0 single player, 1 client, 2 server),
   not a loading-state flag. Gating on a nonzero value would disable single
   player. Current mission, scene and simulation delta govern activity here.

Game memory uses four-byte address slots throughout. All six hooks are CHAIN
links, and the public SDK generates the ARM64 bindings, including the fall
handler's stack arguments and the eye helper's game-visible output buffers.
Drowning consumes a queued weapon hit before supplying an environmental hit
to `handle_blows`; its scripts are rechecked for death, respawn, surfacing or a
mission clear. Damage bypasses armour as Fire Damage does; normal difficulty,
death scripts and network events still run. The self attacker follows the
engine's fall attribution. Water loads before Hit Vibration so hits reach it.

The mission hook shares a target with Depth Texture Shadows, Engine Parity,
Fire Damage and diagnostics; the speed hook shares a target with Touch Combat
and Engine Parity. Current declarations are CHAIN. Older launchers with the
shadow hook declared REPLACE require the corrected launcher to combine them.

## Verification

Verified 2026-10-25: the host behavior checks passed with undefined-behavior
sanitization. The existing remote NDK built and admitted both package objects
(ARMv7: 11,696 bytes; ARM64: 42,488 bytes). Automatic compatibility generated
six hook bindings, one game-call binding and one game-address conversion.
The installable ZIP was checked for root layout, current manifest and both
ELF architectures. No APK was rebuilt and no tools were installed.

The host checks live in the launcher development checkout:

```sh
python3 tests/water/check.py
```

The host harness compiles the production source with undefined-behavior checks
and a mock engine using four-byte game pointers. It covers the mouth boundary,
tilted/scaled/mounted heads, breath timing at several frame rates, fractional
exhaustion, surfacing, pause/stalls, respawn and mission reuse, client/server
gates, combat-hit preservation, scripted state changes, wet/dry fall behavior,
sink/jump behavior, hook registration failures and argument/result forwarding.
The dual-ABI public build additionally checks generated bindings, object
relocations and allowed imports against the package loader.

Hardware checks still required: standard and custom faces at the water line,
crouching/attacking/mounted poses, high-speed water entry and shallow landings,
death/mission triggers under difficulty settings, Touch Combat/Engine Parity/
Fire Damage/Hit Vibration combinations, off-camera crowds, and multiplayer
authority/prediction on ARMv7 and translated ARM64. No device or APK test is
implied by the host checks.
