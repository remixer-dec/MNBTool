---
name: mount-and-blade-warband-engine-mod-maker
description: Create or update native Mount & Blade Warband engine mods for M&BTool with the public ModFramework SDK. Use for mod manifests, game function or import hooks, and symbol or assembly lookups in the original ARMv7 game library.
---

# Mount & Blade Warband Engine Mod Maker for M&BTool

## Get the original game library

Check the paths the user provided first. Otherwise, search from the workspace root
or a folder the user points you to. For example, run:

```sh
rg --files --hidden --no-ignore -g '!**/.git/**' -g 'libMBExpMobile.so'
```

Use `readelf -h` or `llvm-readelf -h` to confirm the file is a little endian `ELF32`
shared object with `Machine: ARM`. Use the ARMv7 library for the game build the mod
targets, including when the mod also builds for translated ARM64. If several copies
fit the architecture, confirm which build the user is targeting.

If the library is missing, ask the user for `libMBExpMobile.so` from their ARMv7 APK
or for the local path to that APK. Explain that APKs are ZIP archives and the file is
normally at `lib/armeabi-v7a/libMBExpMobile.so`. For a supplied APK, check the entry
with `unzip -l` and extract it to a local work directory outside the mod package and SDK:

```sh
unzip -p "$armv7_apk" lib/armeabi-v7a/libMBExpMobile.so > "$lookup_dir/libMBExpMobile.so"
```

Check the extracted ELF header before using the file. While waiting for the library,
read the SDK and work on parts of the mod that can be written without it. Leave
game offsets, signatures, and expected bytes to verify against the supplied file.

## Find symbols and inspect the original assembly

Use the existing Android NDK LLVM tools if available. They are in
`$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/<host-tag>/bin`. GNU `nm` and `readelf`
can also read the ELF tables. Use an ARM capable build such as
`arm-linux-gnueabi-objdump` if `llvm-objdump` is unavailable. Check `objdump -i`
before using a generic host `objdump` for ARM disassembly.

### Make a local lookup index

Set `game_so` to the verified library and `lookup_dir` to a local work directory
for that game build. Keep the index outside the SDK and mod package. Save the raw
and readable symbol names and the assembly as text you can search again later.

```sh
mkdir -p "$lookup_dir"
llvm-nm --dynamic --defined-only --numeric-sort --demangle "$game_so" > "$lookup_dir/dynamic-symbols.txt"
llvm-readelf --symbols --wide "$game_so" > "$lookup_dir/elf-symbols.txt"
llvm-objdump -d --demangle "$game_so" > "$lookup_dir/assembly.txt"
```

Set `symbol_query` to a function or class name, or to an address as it appears in the
listing. Search with `rg -n -F -e "$symbol_query" "$lookup_dir"/*.txt`. Use a separate
directory or rebuild the index when you switch to a different game library.

Use demangled names to find candidates and keep the raw mangled name for symbol
lookups in the mod. A stripped library may have only dynamic symbols. A missing
name does not establish an address. Use known call sites or other evidence from
the same binary to locate internal functions.

### Inspect a function or call site

Set `start` and `stop` to the ELF virtual addresses that bound the function or call
site you want to inspect. Use the even instruction address for an ARM ELF symbol
whose low bit marks a Thumb entry. Preserve the instruction mode when reading the
listing.

```sh
llvm-objdump -d --demangle --start-address="$start" --stop-address="$stop" "$game_so"
```

Check the entry, nearby callers, and argument and return handling before choosing
a hook or prototype. An import hook needs a call through the caller's PLT/GOT.
Inspect its call site and relocations with `llvm-readelf --relocations --wide`.
A function hook needs a verified callable entry. A branch destination inside the
function does not qualify. Keep ELF virtual addresses separate from raw file offsets.
If you need to read original bytes directly, use `llvm-readelf --program-headers`
to find the file backed `LOAD` segment and calculate `p_offset + (address - p_vaddr)`
for that segment.

## Write the mod with the public SDK

- Read the [ModFramework README](../../README.md) for the manifest, game build
  declarations, hook and patch rules, game memory access, and dual ABI builds.
  Use [mod_api.h](../../mod_api.h) for exact API types and entry point signatures.
- Read the [demo mod source](../../demo-mod/mod.cpp) and its
  [manifest](../../demo-mod/manifest.json) together for a working `CHAIN` import
  hook. The README's **Automatic game hooks** example covers game function hooks.
- Keep game addresses in `uint32_t` and use typed field reads for game memory.
  Pointer slots stay four bytes on both targets. Use `uintptr_t` for native
  addresses and count/index accessors for game pointer collections.
- Build native packages with [build-mod.sh](../../build-mod.sh) using the ABI the
  user needs. Use `ANDROID_ABI=both` when the package targets both launchers.
