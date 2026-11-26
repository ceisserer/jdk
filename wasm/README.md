# HotSpot Zero on WebAssembly (wasm32-emscripten)

Experimental port of the HotSpot **Zero** interpreter and the `java.base` native
libraries to `wasm32-unknown-emscripten`: one Wasm module, flat linear memory,
pthreads (Web Workers), Serial GC, statically linked JDK.

Status: `java -version` and a Hello World (lambdas/indy, streams, long/double,
Java threads + `synchronized`, exceptions, GC) run under Node, from a jlinked
`lib/modules` image with a CDS archive (Hello in about 1.1 s with `-Xmx64m`).
See `../AGENTS.md` for details, pitfalls and next steps.

## Prerequisites
- emsdk (`./emsdk install latest && ./emsdk activate latest`), default `~/emsdk`
- host: clang, autoconf, automake, libtool, Node, and header packages
  `libasound2-dev libcups2-dev libfontconfig1-dev` (configure checks only)
- boot JDK (N-1). Do not set `BUILD_JDK`: the build creates one from this tree,
  which `jlink` needs (it must be the exact same build)

## Build and run
```bash
export BOOT_JDK=/path/to/jdk-27
wasm/build-libffi.sh        # libffi for wasm32 -> ~/wasm-sysroot
wasm/configure.sh           # -> build/linux-wasm32-zero-release
wasm/build.sh               # libjvm.a, java.base static libs, exploded java.base
wasm/link.sh                # -> build/linux-wasm32-zero-release/wasm/java.{js,wasm}
wasm/image.sh               # -> build/linux-wasm32-zero-release/wasm-image (lib/modules)
wasm/run.sh -version

# Hello World with a CDS archive (CDS needs a JAR, not a class directory)
javac -d /tmp/hello wasm/test/Hello.java && jar cf /tmp/hello.jar -C /tmp/hello .
wasm/cds.sh /tmp/hello.jsa -cp /tmp/hello.jar Hello
wasm/run.sh -Xmx64m -XX:SharedArchiveFile=/tmp/hello.jsa -cp /tmp/hello.jar Hello
```
All paths can be overridden, see `env.sh`.

## Layout
| Path | Purpose |
|---|---|
| `env.sh` | shared settings (paths, emsdk environment) |
| `build-libffi.sh`, `configure.sh`, `build.sh`, `link.sh`, `run.sh` | build pipeline |
| `image.sh` | jmod + jlink: `java.base` runtime image with `lib/modules` |
| `cds.sh` | training run + `-Xshare:dump` for an application's CDS archive |
| `compat/include/` | headers Emscripten lacks (`sys/eventfd.h`, `sys/sendfile.h`, `sys/inotify.h`, `linux/fs.h`) |
| `compat/wasm_compat.c` | stub/fallback implementations linked into the launcher |
| `js/env-pre.js` | passes the Node environment to `getenv()` |
| `js/killer.js` | ends hung runs (failed VM init does not exit) |
| `tools/topprof.py` | summarizes `node --cpu-prof` profiles per thread |
| `test/Hello.java` | smoke test |
