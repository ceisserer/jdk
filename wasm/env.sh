# Common settings for the wasm32 (HotSpot Zero + Emscripten) build scripts.
# Source this file; every variable can be overridden from the environment.

WASM_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
JDK_SRC=${JDK_SRC:-$(cd "$WASM_DIR/.." && pwd)}       # OpenJDK source tree to build
EMSDK=${EMSDK:-$HOME/emsdk}                             # emsdk checkout (./emsdk install latest)
WASM_SYSROOT=${WASM_SYSROOT:-$HOME/wasm-sysroot}        # libffi (+ empty libatomic/librt) for wasm32
BOOT_JDK=${BOOT_JDK:-}                                  # JDK N-1 (or N) for the host
BUILD_JDK=${BUILD_JDK:-}                                # host JDK of the SAME version as the tree; empty = configure builds one
CONF_NAME=${CONF_NAME:-linux-wasm32-zero-release}
BUILD_DIR=$JDK_SRC/build/$CONF_NAME
OUT_DIR=${OUT_DIR:-$BUILD_DIR/wasm}                     # linked java.js / java.wasm
JAVA_HOME_WASM=${JAVA_HOME_WASM:-$BUILD_DIR/jdk}        # exploded image used as JAVA_HOME at runtime

if [ ! -f "$EMSDK/emsdk_env.sh" ]; then
  echo "emsdk not found at $EMSDK (set EMSDK)" >&2
  return 1 2>/dev/null || exit 1
fi
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
EM_LLVM_BIN=$EMSDK/upstream/bin

# The JDK build refuses to run when these are set (they leak into the boot JDK).
unset JAVA_TOOL_OPTIONS _JAVA_OPTIONS
