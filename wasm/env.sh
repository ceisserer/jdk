# Common settings for the wasm32 (HotSpot Zero + Emscripten) build scripts.
# Source this file; every variable can be overridden from the environment.

WASM_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
JDK_SRC=${JDK_SRC:-$(cd "$WASM_DIR/.." && pwd)}       # OpenJDK source tree to build
EMSDK=${EMSDK:-$HOME/emsdk}                             # emsdk checkout (./emsdk install latest)
WASM_SYSROOT=${WASM_SYSROOT:-$HOME/wasm-sysroot}        # libffi (+ empty libatomic/librt) for wasm32
BOOT_JDK=${BOOT_JDK:-}                                  # JDK N-1 (or N) for the host
BUILD_JDK=${BUILD_JDK:-}                                # leave empty: the build then creates one from this tree, which wasm/image.sh needs
CONF_NAME=${CONF_NAME:-linux-wasm32-zero-release}
BUILD_DIR=$JDK_SRC/build/$CONF_NAME
OUT_DIR=${OUT_DIR:-$BUILD_DIR/wasm}                     # linked java.js / java.wasm
# Runtime JAVA_HOME: the jlinked image from wasm/image.sh if present, else the
# exploded build (much slower startup: every class is a separate file read).
if [ -z "$JAVA_HOME_WASM" ]; then
  if [ -f "$BUILD_DIR/wasm-image/lib/modules" ]; then JAVA_HOME_WASM=$BUILD_DIR/wasm-image; else JAVA_HOME_WASM=$BUILD_DIR/jdk; fi
fi

if [ ! -f "$EMSDK/emsdk_env.sh" ]; then
  echo "emsdk not found at $EMSDK (set EMSDK)" >&2
  return 1 2>/dev/null || exit 1
fi
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
EM_LLVM_BIN=$EMSDK/upstream/bin

# The JDK build refuses to run when these are set (they leak into the boot JDK).
unset JAVA_TOOL_OPTIONS _JAVA_OPTIONS
