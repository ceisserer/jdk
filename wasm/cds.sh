#!/bin/bash
# Create a static CDS archive for an application by running it once in Wasm:
#   wasm/cds.sh app.jsa -cp app.jar Main [args...]
# Step 1 records the loaded classes (-XX:DumpLoadedClassList), step 2 dumps the
# archive (-Xshare:dump). Use it with:
#   wasm/run.sh -XX:SharedArchiveFile=app.jsa -cp app.jar Main
# The classpath must consist of JAR files (CDS rejects non-empty directories).
set -e
[ $# -ge 2 ] || { echo "usage: $0 <archive.jsa> <java args...>" >&2; exit 1; }
ARCHIVE=$1; shift
CLASSLIST=${ARCHIVE%.jsa}.classlist
# Pick out -cp/-classpath for the dump step.
CP_ARGS=(); ARGS=("$@")
for ((i = 0; i < ${#ARGS[@]}; i++)); do
  case "${ARGS[$i]}" in -cp|-classpath|--class-path) CP_ARGS=(-cp "${ARGS[$((i+1))]}");; esac
done
"$(dirname "$0")/run.sh" -Xshare:off -XX:DumpLoadedClassList="$CLASSLIST" "$@"
"$(dirname "$0")/run.sh" -Xshare:dump -XX:SharedClassListFile="$CLASSLIST" -XX:SharedArchiveFile="$ARCHIVE" "${CP_ARGS[@]}"
[ -s "$ARCHIVE" ] || { echo "CDS dump failed" >&2; exit 1; }
echo "archive: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1), $(grep -c '^[^#@]' "$CLASSLIST") classes)"
