#!/bin/bash
# Build the static libjvm.a and the java.base native static libs (plus the exploded
# java.base classes used as JAVA_HOME), and compile the static launcher's main.o.
# Extra arguments are passed to make, e.g. ./build.sh MAKE_ARGS=-k to collect all
# compile errors (see $BUILD_DIR/make-support/failure-logs/).
set -e
source "$(dirname "$0")/env.sh"
cd "$JDK_SRC"
make CONF="$CONF_NAME" hotspot-zero-static-libs java.base-static-libs "$@"

# Only for main.o: the stock static-launcher link passes GNU ld flags
# (--disable-new-dtags, --exclude-libs) that wasm-ld rejects, so that step is
# expected to fail; wasm/link.sh does the real link.
MAIN_O=$BUILD_DIR/support/static-native/launcher/main.o
# The target is "static-launchers" on mainline, "static-launcher" in JDK 25.
if grep -q "TARGET := static-launchers," make/Main.gmk; then LAUNCHER_TARGET=static-launchers; else LAUNCHER_TARGET=static-launcher; fi
make CONF="$CONF_NAME" $LAUNCHER_TARGET-only > "$BUILD_DIR/static-launcher.log" 2>&1 || true
rm -f "$JDK_SRC/a.out.js" "$JDK_SRC/a.out.wasm" "$JDK_SRC/a.wasm"
[ -f "$MAIN_O" ] || { echo "launcher main.o was not built, see $BUILD_DIR/static-launcher.log" >&2; exit 1; }
echo "build done; next: wasm/link.sh"
