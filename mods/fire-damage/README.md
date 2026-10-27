# Fire Damage

A standalone native mod, listed in the online library. **Not bundled in the APK**
or enabled by a graphics preset.

From the public MNBTool repository root:

```sh
ANDROID_ABI=both ./modframework/build-mod.sh mods/fire-damage
```

In the launcher development checkout (`build-mods.sh` only bundles `mods/`):

```sh
ANDROID_ABI=both ./public/build-mod.sh optional-mods/fire-damage
```

Import the resulting `build/` folder (manifest and `.o` objects) through the mod
manager, enable Fire Damage, and restart the game. Flames take 3 health every half
second before the game's difficulty and mission rules (no setting): the game's
received-damage message rounds each hit after difficulty, and smaller hits read 0.
Fire Damage loads before Hit Vibration: an earlier function-hook link is the outer one,
so its hits pass through Vibration's hook.

Fire Damage and Depth Texture Shadows both chain `Mission::tick` (`0xe4d28`).
Older launchers incorrectly declare the shadow hook as a replacement and reject
the combination. Use a launcher with its corrected `CHAIN` declaration to enable
both; updating the Fire Damage package alone cannot correct built-in metadata.

Flames on scene props, torches/carried items and scripted particle bursts affect
people and horses whose bodies touch them. Detection uses the module's particle
system and mesh names (`fire`, `flame`, `burn`, `torch`, `candle`, `brazier`), so
reordered particle IDs and custom effects using the existing fire meshes work.
Smoke, sparks, glow layers and fireflies are excluded. Purely painted fire textures
and custom effects with none of these names cannot be identified automatically.

Checks run every half second of mission simulation. World transforms, emitter
spread and surviving particles define each flame's bounds, with only the inner
fifth of a particle's size (its visible core) counted; height matters.
Overlapping particle layers apply one damage rate. Live emitters remain hazardous
at low particle detail, and finished effects disappear from detection. No camera
or rendering hook is involved. Damage passes through the game's normal hit/death
handling; pending combat hits are consumed first. Multiplayer clients leave damage
to the host/server running the mod. A fire hit is credited as the game credits a
fall: to the burning agent itself, or to a horse's living rider, so death scripts
always get a valid killer.

Implementation offsets are verified against the original Android engine. Game
addresses, vectors and strings retain their four-byte layout on translated ARM64.
Native object/build checks do not replace an in-game check of flame reach, carried
torches, mission damage scripts and multiplayer replication.
