# HotSpot Zero on WebAssembly (wasm32-emscripten)

Experimental port of the HotSpot **Zero** interpreter and the `java.base` native
libraries to `wasm32-unknown-emscripten`: one Wasm module, flat linear memory,
pthreads (Web Workers), Serial GC, statically linked JDK.

Status: on jdk25u (25.0.6) `java -version` and a Hello World (lambdas/indy,
streams, long/double, Java threads + `synchronized`, exceptions, GC) run under
Node. On this mainline branch, HotSpot compiles for wasm32; the full chain
has not been built yet (it needs a JDK 27 boot JDK). See `../AGENTS.md` for
details, pitfalls and next steps.

## Prerequisites
- emsdk (`./emsdk install latest && ./emsdk activate latest`), default `~/emsdk`
- host: clang, autoconf, automake, libtool, Node, and header packages
  `libasound2-dev libcups2-dev libfontconfig1-dev` (configure checks only)
- boot JDK (N-1) and ideally a build JDK of the tree's own version

## Build and run
```bash
export BOOT_JDK=/path/to/jdk-27 BUILD_JDK=/path/to/jdk-28   # BUILD_JDK optional
wasm/build-libffi.sh        # libffi for wasm32 -> ~/wasm-sysroot
wasm/configure.sh           # -> build/linux-wasm32-zero-release
wasm/build.sh               # libjvm.a, java.base static libs, exploded java.base
wasm/link.sh                # -> build/linux-wasm32-zero-release/wasm/java.{js,wasm}
wasm/run.sh -version
javac -d /tmp/hello wasm/test/Hello.java && wasm/run.sh -cp /tmp/hello Hello
```
All paths can be overridden, see `env.sh`.

## Layout
| Path | Purpose |
|---|---|
| `env.sh` | shared settings (paths, emsdk environment) |
| `build-libffi.sh`, `configure.sh`, `build.sh`, `link.sh`, `run.sh` | build pipeline |
| `compat/include/` | headers Emscripten lacks (`sys/eventfd.h`, `sys/sendfile.h`, `sys/inotify.h`, `linux/fs.h`) |
| `compat/wasm_compat.c` | stub/fallback implementations linked into the launcher |
| `js/env-pre.js` | passes the Node environment to `getenv()` |
| `js/killer.js` | ends hung runs (failed VM init does not exit) |
| `tools/topprof.py` | summarizes `node --cpu-prof` profiles per thread |
| `test/Hello.java` | smoke test |
