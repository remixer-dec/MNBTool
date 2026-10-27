#!/bin/sh
# Builds a native mod package for the launcher:
#
#   ./build-mod.sh <mod-dir> [<out-dir>]        (default out: <mod-dir>/build)
#
# <mod-dir> holds manifest.json and one source file: mod.cpp or mod.c (also
# accepted under native/). ANDROID_ABI=both builds one package for both launchers.
# ARM64 game-hook bindings are generated from ARMv7 compiler contracts. The
# source is compiled into relocatable objects, unwind indexes are stripped, and each object is
# checked against the launcher's loader rules, so a mod that would be refused on
# the device fails here. The package (manifest.json, mod.armv7.o) goes to <out-dir>.
#
# Any compiler for 32-bit ARM works; the first one found is used unless set:
#   CXX       compiler: Android NDK clang (ANDROID_NDK_ROOT), clang++, or a GCC
#             ARM cross compiler (arm-linux-gnueabi-g++, arm-none-eabi-g++)
#   OBJCOPY   objcopy that reads ARM ELF (llvm-objcopy, or the GCC toolchain's)
#   READELF   readelf (llvm-readelf, the GCC toolchain's, or readelf)
#   CXXFLAGS  extra compiler flags, e.g. "-I../include -Werror"
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

die() {
    echo "build-mod: $*" >&2
    exit 1
}

# The first argument that names an executable command, as a path.
first_tool() {
    for candidate in "$@"; do
        [ -n "$candidate" ] && command -v "$candidate" 2>/dev/null && return 0
    done
    return 1
}

[ $# -ge 1 ] && [ $# -le 2 ] || die "usage: $0 <mod-dir> [<out-dir>]"
MOD=${1%/}
OUT=${2:-$MOD/build}
[ -f "$MOD/manifest.json" ] || die "$MOD/manifest.json is missing"
SOURCE=
for candidate in "$MOD/mod.cpp" "$MOD/mod.c" "$MOD/native/mod.cpp" "$MOD/native/mod.c"; do
    if [ -f "$candidate" ]; then
        SOURCE=$candidate
        break
    fi
done
[ -n "$SOURCE" ] || die "$MOD has no mod.cpp or mod.c"
ABI=${ANDROID_ABI:-armeabi-v7a}
COMPAT_MODE=${MF_COMPAT_MODE:-automatic}
case $COMPAT_MODE in
    automatic|native) ;;
    *) die "unsupported MF_COMPAT_MODE: $COMPAT_MODE (automatic or native)" ;;
esac
if [ "$ABI" = both ]; then
    mkdir -p "$OUT"
    BOTH_STAGE=$(mktemp -d "$OUT/.build-both.XXXXXX") || die "cannot create build staging directory"
    trap 'rm -rf "$BOTH_STAGE"' EXIT HUP INT TERM
    ANDROID_ABI=armeabi-v7a "$HERE/build-mod.sh" "$MOD" "$BOTH_STAGE/armv7"
    ANDROID_ABI=arm64-v8a "$HERE/build-mod.sh" "$MOD" "$BOTH_STAGE/arm64"
    cp "$BOTH_STAGE/armv7/mod.armv7.o" "$BOTH_STAGE/arm64/mod.arm64.o" "$OUT/"
    cp "$BOTH_STAGE/armv7/manifest.json" "$OUT/manifest.json"
    if [ -f "$BOTH_STAGE/armv7/settings.html" ]; then
        cp "$BOTH_STAGE/armv7/settings.html" "$OUT/settings.html"
    fi
    echo "built $OUT (ARMv7 + ARM64 package; compatibility=$COMPAT_MODE)"
    exit 0
fi
case $ABI in
    armeabi-v7a) OBJECT_NAME=mod.armv7.o ;;
    arm64-v8a) OBJECT_NAME=mod.arm64.o ;;
    *) die "unsupported ANDROID_ABI: $ABI" ;;
esac

# --- Tools -------------------------------------------------------------------

if [ -z "${CXX:-}" ]; then
    NDK=${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}
    NDK_CLANG=
    if [ -n "$NDK" ]; then
        # An NDK may carry several hosts' toolchains: take the one that runs here.
        for candidate in "$NDK"/toolchains/llvm/prebuilt/*/bin/clang++; do
            if "$candidate" --version 2>/dev/null | grep -qi clang; then
                NDK_CLANG=$candidate
                break
            fi
        done
    fi
    CXX=$(first_tool "$NDK_CLANG" clang++ arm-linux-gnueabi-g++ arm-none-eabi-g++) ||
        die "no compiler found; set CXX (or ANDROID_NDK_ROOT)"
fi
CXX_PATH=$(command -v "$CXX") || die "compiler not found: $CXX"
TOOL_DIR=$(dirname "$CXX_PATH")
if "$CXX" --version 2>/dev/null | grep -qi clang; then
    # Same target as the launcher's own libraries (armeabi-v7a, soft-float calls).
    if [ "$ABI" = arm64-v8a ]; then
        # Safe stack: locals whose address may escape live on the launcher's
        # per-thread stack in the game window, so the game can address them;
        # sp stays on the thread's own stack, which Android checks.
        ARCH_FLAGS="--target=aarch64-linux-android24 -mno-outline-atomics
            -fsanitize=safe-stack -mllvm -safestack-use-pointer-address"
    else
        ARCH_FLAGS="--target=armv7a-linux-androideabi24 -mfloat-abi=softfp -marm"
    fi
    OBJCOPY=${OBJCOPY:-$(first_tool "$TOOL_DIR/llvm-objcopy" llvm-objcopy)} || die "no llvm-objcopy found; set OBJCOPY"
    READELF=${READELF:-$(first_tool "$TOOL_DIR/llvm-readelf" llvm-readelf readelf)} || die "no readelf found; set READELF"
else
    [ "$ABI" = armeabi-v7a ] || die "ARM64 builds require the existing NDK clang toolchain"
    ARCH_FLAGS="-march=armv7-a -mfloat-abi=softfp -mfpu=neon -marm"
    PREFIX=${CXX_PATH%g++}
    OBJCOPY=${OBJCOPY:-$(first_tool "${PREFIX}objcopy" llvm-objcopy)} || die "no ARM objcopy found; set OBJCOPY"
    READELF=${READELF:-$(first_tool "${PREFIX}readelf" readelf llvm-readelf)} || die "no readelf found; set READELF"
fi

# --- Compile -----------------------------------------------------------------

# ARM mode (no Thumb relocations), no PIC/GOT, nothing that needs a runtime:
# no exceptions, RTTI, unwind tables, stack protector, COMMON symbols or libc.
COMMON_FLAGS="-c -O2 -Wall -Wextra -fno-pic -fno-pie -fno-common -fno-unwind-tables
    -fno-asynchronous-unwind-tables -fno-stack-protector -ffreestanding"
case $SOURCE in
    *.c) LANGUAGE_FLAGS="-x c -std=c11" ;;
    *) LANGUAGE_FLAGS="-std=c++17 -fno-exceptions -fno-rtti" ;;
esac
mkdir -p "$OUT"
BUILD_STAGE=$(mktemp -d "$OUT/.build-mod.XXXXXX") || die "cannot create build staging directory"
trap 'rm -rf "$BUILD_STAGE"' EXIT HUP INT TERM
OBJECT=$BUILD_STAGE/$OBJECT_NAME
COMPILE_SOURCE=$SOURCE
if [ "$ABI" = arm64-v8a ] && [ "$COMPAT_MODE" = automatic ]; then
    PYTHON=${PYTHON:-python3}
    command -v "$PYTHON" >/dev/null 2>&1 || die "automatic ARM64 bindings require Python 3.9+; set PYTHON"
    case $SOURCE in
        *.c) VIEW_NAME=mod.c ;;
        *) VIEW_NAME=mod.cpp ;;
    esac
    COMMON_FLAGS="$COMMON_FLAGS -DMF_TRANSLATED_ARM64=1"
    COMPILE_SOURCE=$BUILD_STAGE/$VIEW_NAME
    # The compiler producer gets no native target flags: it always derives the
    # game ABI from ARMv7. Imports and platform calls keep their native types.
    # shellcheck disable=SC2086
    "$PYTHON" "$HERE/generate-bindings.py" --compiler "$CXX" --source "$SOURCE" \
        --output "$COMPILE_SOURCE" -- $LANGUAGE_FLAGS -ffreestanding -I"$HERE" ${CXXFLAGS:-}
fi
# shellcheck disable=SC2086  # flag lists split on purpose
"$CXX" $ARCH_FLAGS $COMMON_FLAGS $LANGUAGE_FLAGS -I"$HERE" -I"$(dirname "$COMPILE_SOURCE")" -I"$(dirname "$SOURCE")" \
    ${CXXFLAGS:-} "$COMPILE_SOURCE" -o "$OBJECT"
# ARM EHABI emits an unwind index even without unwind tables; the loader maps
# only code and data sections, and nothing in a mod unwinds.
if [ "$ABI" = armeabi-v7a ]; then
    "$OBJCOPY" --remove-section=.ARM.exidx --remove-section=.rel.ARM.exidx "$OBJECT"
fi

# --- Check (mirrors the launcher's loader) --------------------------------------

SIZE=$(wc -c < "$OBJECT" | tr -d ' ')
[ "$SIZE" -le 16777216 ] || die "$OBJECT is larger than 16 MiB"

HEADER=$("$READELF" -h "$OBJECT")
if [ "$ABI" = arm64-v8a ]; then
    echo "$HEADER" | grep -Eq 'Class:[ 	]+ELF64' || die "not a 64-bit ELF object"
    echo "$HEADER" | grep -Eq 'Machine:[ 	]+AArch64[ 	]*$' || die "not an AArch64 object"
else
    echo "$HEADER" | grep -Eq 'Class:[ 	]+ELF32' || die "not a 32-bit ELF object"
    echo "$HEADER" | grep -Eq 'Machine:[ 	]+ARM[ 	]*$' || die "not a 32-bit ARM object"
fi
echo "$HEADER" | grep -q 'little endian' || die "not a little-endian object"
echo "$HEADER" | grep -Eq 'Type:[ 	]+REL ' || die "not a relocatable object: compile with -c and do not link"
echo "$HEADER" | grep -Eq 'Number of program headers:[ 	]+0' || die "object has program headers"
FLAGS=$(echo "$HEADER" | sed -n 's/^[ 	]*Flags:[ 	]*\(0x[0-9a-fA-F]*\).*/\1/p')
[ -n "$FLAGS" ] || die "missing ELF flags"
if [ "$ABI" = arm64-v8a ]; then
    [ $(($FLAGS)) -eq 0 ] || die "unsupported AArch64 ELF flags"
else
    [ $(($FLAGS >> 24)) -eq 5 ] || die "not an ARM EABI version 5 object"
    [ $(($FLAGS & 0x400)) -eq 0 ] || die "hard-float object: armeabi-v7a passes floats in core registers (-mfloat-abi=softfp)"
fi

# Allocated sections: code and data (PROGBITS) or zero-filled (NOBITS) only.
SECTIONS=$("$READELF" -SW "$OBJECT" | awk -v abi="$ABI" '
    /^[ \t]*\[[ \t]*[0-9]+\]/ {
        line = $0
        sub(/^[^]]*][ \t]*/, "", line)
        n = split(line, field, /[ \t]+/)
        if (n < 9) next                     # the null section
        name = field[1]; type = field[2]
        flags = (n >= 10) ? field[7] : ""
        if (abi == "armeabi-v7a" && type == "RELA") print "problem: RELA relocations (" name ") are unsupported on ARM32"
        if (abi == "arm64-v8a" && type == "REL") print "problem: REL relocations (" name ") are unsupported on ARM64"
        if (flags !~ /A/) next
        print "alloc: " name
        if (type != "PROGBITS" && type != "NOBITS")
            print "problem: section " name " has type " type "; the loader maps only code and data (unwind index? static constructors?)"
        if (flags ~ /T/) print "problem: thread-local section " name
        if (flags ~ /W/ && flags ~ /X/) print "problem: writable executable section " name
    }')
ALLOCATED=" $(echo "$SECTIONS" | sed -n 's/^alloc: //p' | tr '\n' ' ') "

# Relocations in allocated sections: the types the loader applies.
RELOCATIONS=$("$READELF" -rW "$OBJECT" | awk -v allocated="$ALLOCATED" -v abi="$ABI" '
    /^Relocation section/ {
        target = $3
        gsub(/\047/, "", target)
        sub(/^\.rela?/, "", target)
        applies = index(allocated, " " target " ") > 0
        next
    }
    applies && abi == "armeabi-v7a" && $3 ~ /^R_ARM_/ {
        if ($3 !~ /^R_ARM_(NONE|ABS32|REL32|CALL|JUMP24|MOVW_ABS_NC|MOVT_ABS|TARGET1|V4BX)$/) {
            hint = $3 ~ /THM/ ? " (Thumb code: build with -marm)" : $3 ~ /GOT|BASE/ ? " (position-independent code: build with -fno-pic)" : ""
            print "problem: relocation " $3 " in " target " is unsupported" hint
        }
    }
    applies && abi == "arm64-v8a" && $3 ~ /^R_AARCH64_/ {
        if ($3 !~ /^R_AARCH64_(NONE|ABS64|ABS32|PREL32|CALL26|JUMP26|ADR_PREL_PG_HI21|ADR_PREL_LO21|ADD_ABS_LO12_NC|LDST(8|16|32|64|128)_ABS_LO12_NC)$/)
            print "problem: relocation " $3 " in " target " is unsupported"
    }
')

# Symbols: the externals the launcher provides, and a mod_start entry point.
# Keep the list synchronized with resolve_permitted_external() in the launcher.
SYMBOLS=$("$READELF" -sW "$OBJECT" | awk -v abi="$ABI" '
    BEGIN {
        split("memcmp memcpy memmove memset strlen strnlen strcmp strncmp strstr " \
              "malloc calloc realloc free clock_gettime " \
              "__aeabi_memcpy __aeabi_memcpy4 __aeabi_memcpy8 " \
              "__aeabi_memmove __aeabi_memmove4 __aeabi_memmove8 " \
              "__aeabi_memset __aeabi_memset4 __aeabi_memset8 " \
              "__aeabi_memclr __aeabi_memclr4 __aeabi_memclr8 " \
              "__aeabi_uidiv __aeabi_idiv __aeabi_uidivmod __aeabi_idivmod __aeabi_lmul", list, " ")
        for (i in list) permitted[list[i]] = 1
        if (abi == "arm64-v8a") permitted["mf_game_pointer_v1"] = 1   # generated game address conversions
        if (abi == "arm64-v8a") permitted["__safestack_pointer_address"] = 1   # safe-stack runtime
    }
    $1 ~ /^[0-9]+:$/ && $1 != "0:" {
        type = $4; binding = $5; index_ = $7; name = $8
        if (binding != "LOCAL" && binding != "GLOBAL" && binding != "WEAK")
            print "problem: symbol " name " has unsupported binding " binding
        if (type == "TLS") print "problem: thread-local symbol " name
        if (index_ == "COM") print "problem: COMMON symbol " name " (initialise it or use -fno-common)"
        if (index_ == "UND" && name != "" && !(name in permitted)) {
            hint = ""
            if (name ~ /^__aeabi_(u?ldivmod|.*2u?lz|u?l2[fd])$/) hint = " (64-bit division or float/64-bit integer conversion)"
            print "problem: imports " name ", which the launcher does not provide" hint
        }
        if (name == "mod_start" && index_ != "UND" && (binding == "GLOBAL" || binding == "WEAK")) print "entry"
    }')
echo "$SYMBOLS" | grep -qx entry || SYMBOLS="$SYMBOLS
problem: no mod_start entry point (MF_MOD_START)"

PROBLEMS=$(printf '%s\n%s\n%s\n' "$SECTIONS" "$RELOCATIONS" "$SYMBOLS" | sed -n 's/^problem: //p' | awk '!seen[$0]++')
if [ -n "$PROBLEMS" ]; then
    echo "$PROBLEMS" | sed "s|^|build-mod: $OBJECT: |" >&2
    rm -f "$OBJECT"
    exit 1
fi

cp "$OBJECT" "$OUT/$OBJECT_NAME"
cp "$MOD/manifest.json" "$OUT/manifest.json"
if [ -f "$MOD/settings.html" ]; then
    PAGE_SIZE=$(wc -c < "$MOD/settings.html" | tr -d ' ')
    [ "$PAGE_SIZE" -gt 0 ] && [ "$PAGE_SIZE" -le 262144 ] || die "settings.html must be 1..256 KiB"
    cp "$MOD/settings.html" "$OUT/settings.html"
fi
echo "built $OUT ($OBJECT_NAME $SIZE bytes, $(basename "$CXX"))"
