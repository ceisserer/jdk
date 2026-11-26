# Spike: Wasm code installed at runtime via dlopen

Experiment for a future bytecode-to-Wasm compiler: can code loaded at runtime
become a method's entry point in HotSpot Zero on wasm32, with threads?

- `spike_add.c`: hand-written stand-in for compiler output. Zero entry points
  (`int entry(Method*, intptr_t, JavaThread*)`) for `Spike$Fast.add(int,int)`
  and `Spike$Fast.count()`, built as an Emscripten side module. Imports
  `wasm_jit_sp`/`wasm_jit_set_sp` from HotSpot (resolved by the dynamic linker
  against the main module, which needs `-sMAIN_MODULE`).
- `src/hotspot/cpu/zero/wasmJitSpike_zero.cpp`: when `WASM_JIT_SPIKE` is set,
  `Method::link_method` loads the module at a safepoint (VM operation) and
  points the method's interpreter entry at a `ZeroEntry` for the symbol.
- `Spike.java`: installs while 3 threads spin, then times 1M calls.

```bash
source wasm/env.sh
emcc -O2 -pthread -sSIDE_MODULE=1 wasm/spike/spike_add.c -o /tmp/spike_add.so
javac -d /tmp/spike wasm/spike/Spike.java
WASM_JIT_SPIKE='Spike$Fast.add=/tmp/spike_add.so:spike_add_entry;Spike$Fast.count=/tmp/spike_add.so:spike_count_entry' \
  wasm/run.sh -Xmx64m -cp /tmp/spike Spike
```

## Results (Node 22, 4 cores)

- Works: correct results from 5 Java threads, `count()` shows the Wasm code ran.
- Install (first `dlopen`, all threads synchronized): about 21 to 28 ms; a
  second symbol from the same module: under 0.3 ms.
- 1M calls from an interpreted loop: 112 ns/call (Wasm callee) vs 245 ns/call
  (interpreted callee). The rest is the interpreted caller's call path.
- The 4-thread phase is not meaningful for the Wasm version: the spike's shared
  atomic counter makes the threads contend.

## Findings

1. **dlopen must run at a safepoint.** With pthreads, `dlopen` makes every
   thread load the module into its own instance, and a thread only does that
   while it waits (futex) and processes its Emscripten mailbox. A thread
   spinning in Java code blocks `dlopen` forever. At a safepoint all Java
   threads are parked, so it completes.
2. That exposed an **upstream Zero bug**: the interpreter had no safepoint polls
   on backward branches (`DO_BACKEDGE_CHECKS` was empty), so a call-free loop
   blocked every safepoint (also GC and VM exit) on native Zero too. Fixed in
   `bytecodeInterpreter.cpp`; regression test `wasm/test/Spin.java`.
3. The poll needs a plain (non-atomic) pre-check: on wasm32 the acquire load in
   `SafepointMechanism::should_process` is an atomic load and slowed loops by up
   to 2.4x.
