#!/bin/bash
# Build libffi for wasm32-emscripten (with pthreads) into $WASM_SYSROOT.
# Needs: autoconf, automake, libtool. LIBFFI_SRC defaults to a fresh clone.
set -e
source "$(dirname "$0")/env.sh"
LIBFFI_SRC=${LIBFFI_SRC:-$JDK_SRC/build/libffi-src}
[ -d "$LIBFFI_SRC" ] || git clone --depth 1 https://github.com/libffi/libffi.git "$LIBFFI_SRC"
cd "$LIBFFI_SRC"
[ -x configure ] || ./autogen.sh
emconfigure ./configure --host=wasm32-unknown-emscripten --prefix="$WASM_SYSROOT" \
  --enable-static --disable-shared --disable-dependency-tracking --disable-builddir \
  --disable-multi-os-directory --disable-raw-api --disable-docs CFLAGS="-O2 -pthread"
emmake make -j"$(nproc)" install
# The JDK's static libs ask for -latomic and -lrt; Emscripten has neither, so provide empty archives.
cd "$WASM_SYSROOT/lib"
for l in atomic rt; do [ -f lib$l.a ] || emar rcs lib$l.a; done
echo "libffi installed in $WASM_SYSROOT"
