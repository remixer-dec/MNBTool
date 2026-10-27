# M&BTool

Unofficial fan launcher for Mount & Blade: Warband on Android, with engine mods,
configurable controls, a mobile patch and translations. Requires your own full copy
of the game. This project is not affiliated with TaleWorlds Entertainment.

- [ModFramework SDK and API guide](modframework/README.md), including a working
  [frame-rate demo](modframework/demo-mod/)
- Example mods: [Fire Damage](mods/fire-damage/README.md) and
  [Water Physics & Drowning](mods/water/README.md)

## Build a mod

The SDK includes its headers, build script and ARM64 binding generator. It requires
an Android NDK and standard Python 3.9+.
From this repository's root:

```sh
export ANDROID_NDK_ROOT=/path/to/android-ndk
ANDROID_ABI=both ./modframework/build-mod.sh modframework/demo-mod
ANDROID_ABI=both ./modframework/build-mod.sh mods/fire-damage
ANDROID_ABI=both ./modframework/build-mod.sh mods/water
```

Each mod's `build/` folder contains its manifest and ARMv7/ARM64 objects. Import
the folder through the launcher's mod manager, or ZIP its contents with
`manifest.json` at the archive root. Enable the mod and restart the game.

Launcher sources may be provided in the future after reorganization.
