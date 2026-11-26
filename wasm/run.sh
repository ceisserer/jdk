#!/bin/bash
# Run the Wasm java launcher under Node, e.g.: ./run.sh -version
# KILL_AFTER (ms, default 120000) ends a hung VM; TIMEOUT (s) is a hard limit.
WASM_RUNTIME_ONLY=1 source "$(dirname "$0")/env.sh"
export JAVA_HOME="$JAVA_HOME_WASM"
export KILL_AFTER=${KILL_AFTER:-120000}
exec timeout "${TIMEOUT:-180}" node -r "$WASM_DIR/js/killer.js" "$OUT_DIR/java.js" "$@"
