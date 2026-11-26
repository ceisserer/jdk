#!/bin/bash
# Create a jlinked java.base runtime image (lib/modules jimage) for wasm32 in
# $BUILD_DIR/wasm-image. Run after wasm/build.sh.
#
# jlink only accepts a target java.base from exactly the same build as itself,
# so this uses the host build JDK that the JDK build creates from this source
# tree (make create-buildjdk; requires configuring WITHOUT BUILD_JDK).
# make java.base-jmod cannot be used: it needs a shared libjvm.so and every
# other jmod, so the jmod is packaged directly from the exploded classes.
set -e
source "$(dirname "$0")/env.sh"
BJ=$BUILD_DIR/buildjdk/jdk/bin
if [ ! -x "$BJ/jlink" ]; then
  (cd "$JDK_SRC" && make CONF="$CONF_NAME" create-buildjdk)
fi
VERSION=$("$BJ/java" -version 2>&1 | sed -n 's/.*version "\(.*\)".*/\1/p')
rm -rf "$BUILD_DIR/wasm-jmods" "$BUILD_DIR/wasm-image"
mkdir -p "$BUILD_DIR/wasm-jmods"
"$BJ/jmod" create --module-version "$VERSION" --target-platform linux-wasm32 \
  --class-path "$BUILD_DIR/jdk/modules/java.base" \
  --config "$BUILD_DIR/support/modules_conf/java.base" \
  --legal-notices "$BUILD_DIR/support/modules_legal/java.base" \
  "$BUILD_DIR/wasm-jmods/java.base.jmod"
"$BJ/jlink" --module-path "$BUILD_DIR/wasm-jmods" --add-modules java.base --output "$BUILD_DIR/wasm-image"
cp "$BUILD_DIR/jdk/lib/jvm.cfg" "$BUILD_DIR/wasm-image/lib/"
echo "image: $BUILD_DIR/wasm-image ($(du -sh "$BUILD_DIR/wasm-image/lib/modules" | cut -f1) lib/modules)"
