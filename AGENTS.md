# Notes for agents and LLMs working on this repository

This is a fork of [openjdk/jdk](https://github.com/openjdk/jdk) (mainline).
The `wasm32-zero` branch ports the HotSpot **Zero** interpreter and the
`java.base` native libraries to WebAssembly (`wasm32-unknown-emscripten`).
Everything else in the tree is upstream OpenJDK; follow upstream conventions
(`doc/building.md`, `doc/hotspot-style.md`).

## Commits

- Commit author: **Clemens Eisserer <ceisserer@gmail.com>**
  (`git -c user.name="Clemens Eisserer" -c user.email=ceisserer@gmail.com commit ...`).
- Do **not** add `Co-Authored-By:` trailers.
- Do not commit build output (`build/` is ignored) or the `a.out.js`, `a.out.wasm`
  and `a.wasm` files that emcc's configure probes leave in the source root.
- Do not commit local workarounds for missing tools, such as relaxing
  `DEFAULT_ACCEPTABLE_BOOT_VERSIONS` in `make/conf/version-numbers.conf` or the
  build-JDK version check in `make/autoconf/boot-jdk.m4`.

## Where things are

- `wasm/`: build pipeline (`build-libffi.sh`, `configure.sh`, `build.sh`,
  `link.sh`, `run.sh`, settings in `env.sh`), compat headers and stubs for APIs
  Emscripten lacks (`wasm/compat/`), Node helpers (`wasm/js/`), a profile
  summarizer (`wasm/tools/topprof.py`) and a smoke test (`wasm/test/Hello.java`).
  Quick start: `wasm/README.md`.
- Source changes are guarded with `#ifdef __EMSCRIPTEN__` (C/C++) or
  `defined(__EMSCRIPTEN__)` (Java templates run through CPP). Keep it that way so
  that other platforms are untouched. Find them with
  `git grep -n __EMSCRIPTEN__ -- src make`.
  - `make/autoconf/platform.m4`: `wasm32` CPU; `*emscripten*` maps to OS `linux`.
  - `src/hotspot/os/linux/os_linux.cpp`: memory reserve/commit/uncommit/release,
    stack guard pages, gettid, sched_getcpu, prctl, sendfile, ELF diagnostics.
  - `src/hotspot/os/posix/os_posix.cpp`: `os::jvm_path()` from `$JAVA_HOME`.
  - `src/hotspot/cpu/zero/zeroInterpreter_zero.cpp`: 8-byte alignment for libffi.
  - `src/hotspot/share/utilities/compilerWarnings.hpp`: libc not `noexcept`.
  - `src/hotspot/share/cds/filemap.{hpp,cpp}`, `src/hotspot/share/code/aotCodeCache.cpp`:
    `intx` overloads (see the pitfalls below).
  - `waitBarrier.hpp`, `waitBarrier_linux.cpp`, `systemMemoryBarrier_linux.cpp`:
    no futex or membarrier syscalls.

## Building and testing

- Mainline needs a **JDK 27 boot JDK**. A cross build also needs a build JDK of
  the tree's own version (`BUILD_JDK`); without one, configure builds it itself,
  which takes much longer.
- `JAVA_TOOL_OPTIONS` and `_JAVA_OPTIONS` must be unset, or configure aborts
  (`wasm/env.sh` unsets them).
- Order: `wasm/build-libffi.sh`, `wasm/configure.sh`, `wasm/build.sh`,
  `wasm/link.sh`, `wasm/run.sh -version`.
- To see every compile error at once instead of stopping at the first:
  `wasm/build.sh MAKE_ARGS=-k`, then read
  `build/<conf>/make-support/failure-logs/*.log`.
- To rebuild one HotSpot object fast, re-run the command in
  `failure-logs/*.cmdline`.
- Verification state:
  - The full chain (configure → run `Hello`) was tested on jdk25u (25.0.6).
  - On mainline only `make hotspot-zero-static-libs` was verified, using a JDK 25
    boot JDK with the version checks relaxed locally. `java.base` was not built,
    because the interim javac needs JDK 27.
- Smoke test: `javac -d /tmp/hello wasm/test/Hello.java && wasm/run.sh -cp /tmp/hello Hello`.
  It should print `thread sum = 10` and end with `gc ok`.

## Pitfalls already found (read before debugging)

1. **Emscripten does not define `__linux__`.** The JDK natives and Java templates
   key a lot on it, so `wasm/configure.sh` adds `-D__linux__`. HotSpot itself uses
   the build's `LINUX` macro.
2. **libffi wasm32 bug:** 8-byte arguments are read as `HEAPU64[addr >>> 3]`,
   which silently rounds an address that is not 8-byte aligned down to the
   previous 8-byte boundary. Zero's 32-bit stack slots are only 4-byte aligned,
   so `jlong`/`double` arguments came through with their 32-bit halves mixed up.
   Zero now copies them, and the result buffer, to 8-byte-aligned storage. Any
   new code that passes 8-byte values through libffi must do the same.
   Symptom: an `Unsafe.compareAndSetInt` CAS loop spinning forever during VM init.
   This should be reported upstream to libffi.
3. **Memory model:** Emscripten's `mmap` cannot place a mapping at a given
   address (with an address it returns EINVAL) and `munmap` only frees whole
   mappings. Hence reserve = anonymous mmap (zeroed, accessible), commit = no-op,
   uncommit = `memset(0)`, partial release = zero and leak. `mprotect` and
   `madvise` do nothing.
4. **Never write into stack guard zones.** The bottom of each pthread stack holds
   Emscripten's stack cookie; zeroing it aborts with "Stack cookie has been
   overwritten" when the thread exits. The guard page functions are no-ops on Wasm.
5. **`intptr_t` is `long` on wasm32** (on arm32 it is `int`). `intx`/`uintx` are
   therefore distinct from `int`/`uint` although `_LP64` is not defined, and
   overload sets guarded by `_LP64` become ambiguous. The fix pattern is
   `#if defined(_LP64) || defined(__EMSCRIPTEN__)`.
6. **musl headers declare nothing `noexcept`.** HotSpot's forbidden-function
   redeclarations must not add `noexcept` (handled like BSD).
7. **`dlsym(RTLD_DEFAULT, ...)` needs `-sMAIN_MODULE=1`.** The static JDK resolves
   JNI functions that way. It makes `java.wasm` about 3.8 MB. A generated table
   of JNI symbols would be the smaller fix.
8. **A failed VM init hangs instead of exiting** (exit while pthreads are alive).
   `wasm/run.sh` preloads `wasm/js/killer.js` (`KILL_AFTER` ms). Do not pipe the
   output into `head`; output can look lost when the process hangs.
9. **"warning: unsupported syscall: __syscall_prlimit64/mprotect/madvise"** messages
   are expected and harmless.
10. **HotSpot needs threads from the start** (VM thread, Reference Handler, etc.).
    Hence `-pthread`, `-sPROXY_TO_PTHREAD` and `-sPTHREAD_POOL_SIZE=24`. In a
    browser this also requires COOP/COEP headers.

## Debugging technique that worked

- `node --cpu-prof --cpu-prof-dir=prof java.js ...` (link with `DEBUG=1` for
  function names), then `wasm/tools/topprof.py prof`. It shows per thread whether
  the VM spins (interpreter and natives hot) or blocks (`emscripten_futex_wait`).
- `-Xlog:class+load,class+init,startuptime,os+thread,gc+init` to see how far
  initialization gets.
- Temporary `fprintf(stderr, ...)` guarded by an environment variable in the
  suspected native function (e.g. `Unsafe_CompareAndSetInt`) found the libffi bug
  quickly. Remove such instrumentation before committing.
- Calling HotSpot's `pss()` from the JS main thread printed nothing, because
  that thread is not attached to the VM.

## Performance baseline (jdk25u, Node 22, 4 cores)

- Hello World takes about 6.5 s; `Create VM` alone takes about 5.7 s. Native
  `-Xint -Xshare:off` needs about 0.12 s.
- Main Java thread during startup:
  - Zero interpreter: about 1.9 s.
  - Waiting on file I/O proxied to the JS main thread (about 450 class files
    loaded from the exploded image): about 1.5 s.
  - pthreads + `ALLOW_MEMORY_GROWTH` overhead (`growMemViews`): about 0.8 s.
  - `memset` of reservations: about 0.4 s.

## Next steps

1. Build `lib/modules` (jimage) and try a CDS archive or AOT cache
   (`-XX:AOTCacheOutput`) from a training run under Node. That is the biggest
   startup win.
2. Use a fixed-size heap (no `ALLOW_MEMORY_GROWTH`) and avoid zeroing large
   reservations.
3. Replace `MAIN_MODULE` with a generated table of JNI symbols.
4. Browser embedding: COOP/COEP headers, a preloaded virtual file system instead
   of `NODERAWFS`, and running in a worker.
5. Later: a bytecode-to-Wasm code tier whose frames live on the Zero stack (like
   the old Shark design), loaded from the AOT cache.
