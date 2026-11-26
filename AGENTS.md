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
  - `src/hotspot/share/cds/aotMetaspace.hpp`: read CDS regions instead of mmap.
  - `src/hotspot/share/utilities/macros.hpp`: `INCLUDE_CDS_JAVA_HEAP` also on
    Emscripten (archived heap objects, otherwise 64-bit only).
  - `src/hotspot/share/cds/aotMetaspace.cpp`: set up archived-heap loading also
    without class space.
  - `src/hotspot/share/cds/aotStreamedHeapLoader.cpp`: 32-bit fixes (klass
    decoding, pointer-sized references, klass word not copied).
  - `src/java.base/unix/native/libnio/fs/UnixNativeDispatcher.c`: bind
    `openat`/`fstatat`/`fdopendir` directly.
  - `src/java.base/share/classes/jdk/internal/util/Architecture.java` and
    `PlatformProps.java.template`: `WASM32` architecture (plus
    `test/jdk/jdk/internal/util/ArchTest.java`).

## Building and testing

- Mainline needs a **JDK 27 boot JDK** (tested: 27.0.1.1 GA from
  `download.java.net`; verify the published `.sha256`).
- **Do not pass `BUILD_JDK`.** The build then creates its own host build JDK from
  this tree (`make create-buildjdk`, a few minutes; `wasm/image.sh` runs it if
  needed). `jlink` only accepts a `java.base` from exactly the same build as
  itself, so an external JDK 28 (e.g. an EA build) can compile `java.base` but
  cannot create the image.
- `JAVA_TOOL_OPTIONS` and `_JAVA_OPTIONS` must be unset, or configure aborts
  (`wasm/env.sh` unsets them).
- Order: `wasm/build-libffi.sh`, `wasm/configure.sh`, `wasm/build.sh`,
  `wasm/link.sh`, `wasm/image.sh`, `wasm/run.sh -version`. Optionally
  `wasm/aot.sh app.aot -cp app.jar Main` for an AOT cache (preferred) or
  `wasm/cds.sh app.jsa -cp app.jar Main` for a classic CDS archive.
- `wasm/run.sh` uses the jlinked image (`build/<conf>/wasm-image`) as `JAVA_HOME`
  when it exists, otherwise the exploded build (`build/<conf>/jdk`).
- To see every compile error at once instead of stopping at the first:
  `wasm/build.sh MAKE_ARGS=-k`, then read
  `build/<conf>/make-support/failure-logs/*.log`.
- To rebuild one HotSpot object fast, re-run the command in
  `failure-logs/*.cmdline`.
- Verification state: the full chain (configure → build → link → image → CDS
  archive / AOT cache → `java -version` and `Hello` with the archive or cache)
  runs on this branch (JDK 28 mainline). Without image, CDS and AOT cache it was
  earlier also verified on jdk25u.
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
10. **musl has no `*64` aliases** (`fstatat64`, `openat64`). Code that looks them
    up with `dlsym` silently gets NULL. libnio now binds `openat`/`fstatat`/
    `fdopendir` directly on Emscripten, as on BSD. Symptom: `InternalError: should
    not reach here` from `UnixNativeDispatcher.fstatat` while the boot layer
    initializes. `-Xlog:exceptions=info` shows where such an exception is thrown.
11. **The stock static-launcher link always fails** (GNU ld flags that `wasm-ld`
    rejects). `wasm/build.sh` runs it only to get `main.o`; the target is
    `static-launchers` on mainline and `static-launcher` in JDK 25.
12. **HotSpot needs threads from the start** (VM thread, Reference Handler, etc.).
    Hence `-pthread`, `-sPROXY_TO_PTHREAD` and `-sPTHREAD_POOL_SIZE=24`. In a
    browser this also requires COOP/COEP headers.
13. **jlink needs a known platform.** The jmod's `--target-platform linux-wasm32`
    is parsed with `jdk.internal.util.Architecture.lookupByName`, which rejects
    unknown names; `WASM32` is now an enum constant (with `isWASM32()` and
    `PlatformProps.TARGET_ARCH_IS_WASM32`). `make java.base-jmod` does not work
    (it needs a shared `libjvm.so` and all other jmods); `wasm/image.sh` packages
    the jmod directly from the exploded classes.
14. **CDS archives are read, not mapped.** Emscripten cannot mmap a file into
    reserved memory, so `AOTMetaspace::use_windows_memory_mapping()` returns true
    on Emscripten (the Windows path, which `read()`s the regions into the
    reserved space). The log line "Mapped static region" is printed either way.
15. **CDS rejects directories on the classpath** ("non-empty directory"); package
    the application as a JAR.
16. **CDS vs AOT cache:** the JDK 24+ "AOT cache" (Leyden) is CDS extended with
    linked classes, resolved invokedynamic sites, method profiles and (newer)
    compiled code. For Zero only the CDS part plus class linking and resolved call
    sites help; profiles and compiled code need a JIT. "AOT code" in the next
    steps means a future Wasm code tier, not this cache. Use the two-step
    workflow (`AOTMode=record`, then `create`; `wasm/aot.sh`): one-step
    `-XX:AOTCacheOutput` starts a child JVM process, which Wasm cannot do.
17. **Archived heap objects on 32-bit.** Upstream compiles them out on all 32-bit
    platforms (`INCLUDE_CDS_JAVA_HEAP` requires `_LP64`), and AOT class linking
    needs them (via the full module graph): "AOT class linking was enabled in
    training run but has been disabled due to incompatible module options".
    Enabling them on Emscripten compiled without errors, but needed these
    runtime fixes, all in the streamed format (JEP 516), which is the default
    without compressed oops:
    - the heap loading mode was only initialized inside `#if INCLUDE_CLASS_SPACE`
      (64-bit only); symptom: divide-by-zero trap in `AOTMappedHeapLoader::get_root`;
    - the writer stores a narrowKlass (offset from the archive base) even where
      headers hold a full `Klass*`; the loader decodes it with `SharedBaseAddress`;
      symptom: out-of-bounds access in `ObjAllocator::initialize`;
    - references are stored pointer-sized (`ssize_t`) but were read as `int64_t`;
    - the copy paths copied the klass word (the narrowKlass) over the correct
      `Klass*`; symptom: "guarantee(... is_oop(loader())) failed: Loader must be
      oop" in `SystemDictionary::register_loader`.
    The writer side needed no changes.

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

## Performance (Node 22, 4 cores, `wasm/test/Hello.java`, wall clock incl. Node)

| Setup | Time |
|---|---|
| exploded build (mainline) | about 7.5 s (`Create VM` about 5.7 s on jdk25u) |
| jimage, no CDS | 1.68 s |
| jimage + CDS | 1.40 s |
| jimage + CDS, `-Xmx64m` (before archived heap objects) | 1.09 s (`Create VM` 0.19 s) |
| jimage only, `-Xmx64m` | 1.43 s (`Create VM` 0.30 s) |
| jimage + CDS with heap objects, `-Xmx64m` | 1.12 s (`Create VM` 0.17 s) |
| jimage + AOT cache, `-Xmx64m` | 1.05 s (`Create VM` 0.16 s; module system init 3 ms vs 59 ms without archive) |
| native HotSpot `-Xint -Xshare:off` (for comparison) | about 0.12 s |

- The numbers above were measured through `wasm/run.sh`, which then still
  sourced `emsdk_env.sh` (about 150 ms per run) and used a pool of 24 workers.
- **Thread pool size dominated the rest.** Every pool worker is a separate JS
  isolate that loads `java.js`/`java.wasm`; on 4 cores, 24 of them delayed
  startup by about 370 ms (the main thread's `readFileSync` of `java.wasm`
  "took" 355 ms in the profile) and exit by about 100 ms. A small pool makes
  extra threads expensive instead (pool 4: Java `main()` 250 ms instead of 65 ms).
  Hello, AOT cache, `-Xmx64m`, internal timeline, best of 3 to 5:

  | Pool | Total | of which exit after `main()` |
  |---|---|---|
  | 24 | 875 ms | 100 ms |
  | 16 | 694 ms | 83 ms |
  | 12 | 636 ms | 84 ms |
  | 12 + `PTHREAD_POOL_DELAY_LOAD` (now default) | 562 to 603 ms | 64 ms |
  | 8 | 808 ms | |
  | 4 | 998 ms | |

- Now (pool 12, delayed pool load, `run.sh` without emsdk): Hello with AOT cache
  and `-Xmx64m` takes **0.65 s** wall clock; `node -e 0` alone takes 0.03 s.
  Phases (internal timeline, about 560 ms total): Node start and parsing
  `java.js` 40 ms; reading/instantiating `java.wasm` and starting the pool until
  C `main()` about 165 to 220 ms; launcher + `Create VM` about 215 to 265 ms
  (`Create VM` about 170 ms); Java `main()` about 65 ms; exit about 50 to 65 ms.
- `-XX:-UsePerfData`, `-XX:+ReduceSignalUsage` and
  `-XX:-AOTCacheParallelRelocation` changed the total only within the noise
  (about ±30 ms).
- How to measure: link with `TIMING=1` (`wasm/js/timing-pre.js`), run
  `wasm/test/Probe.java` with `-Xlog:startuptime:stderr:timemillis`, and pipe the
  output into `wasm/tools/timeline.py` (see the comment in `Probe.java`).
- `-Xmx` matters because the whole maximum heap is reserved, and Emscripten's
  mmap zeroes the full reservation (`Genesis` 0.35 s with the default 512 MB,
  0.07 s with 64 MB).
- Breakdown of the old exploded-build startup (main Java thread):
  - Zero interpreter: about 1.9 s.
  - Waiting on file I/O proxied to the JS main thread (about 450 class files
    loaded from the exploded image): about 1.5 s.
  - pthreads + `ALLOW_MEMORY_GROWTH` overhead (`growMemViews`): about 0.8 s.
  - `memset` of reservations: about 0.4 s.

## Next steps

1. Reduce the cost per worker (each loads all of `java.js`/`java.wasm`): a smaller
   module without `MAIN_MODULE` (its GOT/table setup took about 35 ms on the main
   thread), and fewer HotSpot threads at startup.
2. Avoid zeroing large reservations (reserve lazily, or zero on commit only) and
   try a fixed-size Wasm heap (no `ALLOW_MEMORY_GROWTH`).
3. Replace `MAIN_MODULE` with a generated table of JNI symbols.
4. Browser embedding: COOP/COEP headers, a preloaded virtual file system instead
   of `NODERAWFS`, and running in a worker.
5. Later: a bytecode-to-Wasm code tier whose frames live on the Zero stack (like
   the old Shark design), loaded from the AOT cache.
