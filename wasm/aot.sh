#!/bin/bash
# Create an AOT cache (JDK 24+ "Leyden" CDS: linked classes, archived heap
# objects / full module graph, resolved invokedynamic sites) by running the
# application once in Wasm:
#   wasm/aot.sh app.aot -cp app.jar Main [args...]
# Use it with:
#   wasm/run.sh -XX:AOTCache=app.aot -cp app.jar Main
# Uses the two-step workflow (AOTMode=record, then AOTMode=create): the one-step
# -XX:AOTCacheOutput starts a child JVM process, which Wasm cannot do.
# The classpath must consist of JAR files.
set -e
[ $# -ge 2 ] || { echo "usage: $0 <cache.aot> <java args...>" >&2; exit 1; }
CACHE=$1; shift
CONFIG=${CACHE%.aot}.aotconf
CP_ARGS=(); ARGS=("$@")
for ((i = 0; i < ${#ARGS[@]}; i++)); do
  case "${ARGS[$i]}" in -cp|-classpath|--class-path) CP_ARGS=(-cp "${ARGS[$((i+1))]}");; esac
done
rm -f "$CACHE" "$CONFIG"
"$(dirname "$0")/run.sh" -XX:AOTMode=record -XX:AOTConfiguration="$CONFIG" "$@"
"$(dirname "$0")/run.sh" -XX:AOTMode=create -XX:AOTConfiguration="$CONFIG" -XX:AOTCache="$CACHE" "${CP_ARGS[@]}"
[ -s "$CACHE" ] || { echo "AOT cache creation failed" >&2; exit 1; }
echo "AOT cache: $CACHE ($(du -h "$CACHE" | cut -f1))"
