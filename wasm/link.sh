#!/bin/bash
# Link the static java launcher + libjvm.a + java.base natives into one Wasm module
# ($OUT_DIR/java.js + java.wasm). DEBUG=1 adds Emscripten runtime assertions and
# function names for CPU profiles (node --cpu-prof, see wasm/tools/topprof.py).
# TIMING=1 prints startup phase timestamps (wasm/js/timing-pre.js).
# Extra arguments are passed to em++.
set -e
source "$(dirname "$0")/env.sh"
mkdir -p "$OUT_DIR"
emcc -O2 -pthread -c "$WASM_DIR/compat/wasm_compat.c" -o "$OUT_DIR/wasm_compat.o"

LIBS=()
for l in verify java zip jimage jli net nio syslookup fallbackLinker; do
  LIBS+=("$BUILD_DIR/support/native/java.base/lib$l/static/lib$l.a")
done

DEBUG_FLAGS=()
if [ "${TIMING:-0}" = 1 ]; then
  DEBUG_FLAGS+=(--pre-js "$WASM_DIR/js/timing-pre.js")
fi
if [ "${DEBUG:-0}" = 1 ]; then
  DEBUG_FLAGS+=(-sASSERTIONS=1 --profiling-funcs)
fi

# MAIN_MODULE=1: the static JDK resolves JNI functions with dlsym(RTLD_DEFAULT, ...),
# which Emscripten only supports with dynamic linking enabled.
# PROXY_TO_PTHREAD: run main() in a worker, since HotSpot blocks on locks.
# PTHREAD_POOL_SIZE: workers created at startup. Each is a separate JS isolate
# that loads java.js/java.wasm, so a large pool delays startup and exit (24
# workers: about 950 ms for Hello; 12: about 600 ms). Threads beyond the pool
# get a new worker on demand. POOL_DELAY_LOAD lets main() start before the pool
# is ready. Override with POOL_SIZE=n.
# NODERAWFS: direct host file system access under Node (not for browsers).
em++ -O2 -pthread -o "$OUT_DIR/java.js" \
  "$BUILD_DIR/support/static-native/launcher/main.o" "$OUT_DIR/wasm_compat.o" \
  -Wl,--whole-archive "${LIBS[@]}" "$BUILD_DIR/hotspot/variant-zero/libjvm/objs/static/libjvm.a" -Wl,--no-whole-archive \
  -L"$WASM_SYSROOT/lib" -lffi \
  -sMAIN_MODULE=1 -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=${POOL_SIZE:-12} -sPTHREAD_POOL_DELAY_LOAD=1 \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=2GB \
  -sSTACK_SIZE=2MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB -sNODERAWFS=1 -sEXIT_RUNTIME=1 \
  --pre-js "$WASM_DIR/js/env-pre.js" \
  "${DEBUG_FLAGS[@]}" "$@"
echo "linked $OUT_DIR/java.js"
