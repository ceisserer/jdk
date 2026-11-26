#!/bin/bash
# Configure OpenJDK for HotSpot Zero on wasm32-unknown-emscripten.
# Extra arguments are passed to configure.
set -e
source "$(dirname "$0")/env.sh"
[ -n "$BOOT_JDK" ] || { echo "set BOOT_JDK" >&2; exit 1; }

# Linux configure insists on ALSA/CUPS/fontconfig headers even for java.base-only
# builds. They are never compiled into the wasm build; point at the host headers
# (apt: libasound2-dev libcups2-dev libfontconfig1-dev) through a private dir.
FAKEINC=$JDK_SRC/build/wasm-host-headers
mkdir -p "$FAKEINC"
for d in alsa cups fontconfig; do ln -sfn /usr/include/$d "$FAKEINC/$d"; done

COMPAT_CFLAGS="-pthread -D__linux__ -I$WASM_DIR/compat/include"
ARGS=(
  --openjdk-target=wasm32-unknown-emscripten
  --with-conf-name="$CONF_NAME"
  --with-jvm-variants=zero
  --with-jvm-features=-g1gc,-parallelgc,-jvmti,-management
  --with-toolchain-type=clang
  --with-boot-jdk="$BOOT_JDK"
  --with-libffi="$WASM_SYSROOT"
  --enable-headless-only
  --with-debug-level=${DEBUG_LEVEL:-release}
  --with-native-debug-symbols=none
  --disable-warnings-as-errors
  --disable-precompiled-headers
  --with-zlib=bundled --with-libjpeg=bundled --with-giflib=bundled --with-libpng=bundled
  --with-lcms=bundled --with-freetype=bundled --with-harfbuzz=bundled
  --with-alsa-include="$FAKEINC" --with-alsa-lib="$WASM_SYSROOT/lib"
  --with-cups-include="$FAKEINC" --with-fontconfig-include="$FAKEINC"
  --with-extra-cflags="$COMPAT_CFLAGS" --with-extra-cxxflags="$COMPAT_CFLAGS" --with-extra-ldflags=-pthread
)
[ -n "$BUILD_JDK" ] && ARGS+=(--with-build-jdk="$BUILD_JDK")

cd "$JDK_SRC"
bash configure "${ARGS[@]}" \
  BUILD_CC=clang BUILD_CXX=clang++ BUILD_NM=nm BUILD_AR=ar BUILD_OBJCOPY=objcopy BUILD_STRIP=strip \
  CC=emcc CXX=em++ AR=emar NM="$EM_LLVM_BIN/llvm-nm" OBJCOPY="$EM_LLVM_BIN/llvm-objcopy" STRIP="$EM_LLVM_BIN/llvm-strip" \
  "$@"
# emcc's configure probes drop a.out.js / a.out.wasm / a.wasm into the source root.
rm -f "$JDK_SRC/a.out.js" "$JDK_SRC/a.out.wasm" "$JDK_SRC/a.wasm"
