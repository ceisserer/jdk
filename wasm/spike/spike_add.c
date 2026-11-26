/*
 * Hand-written stand-in for JIT output: Zero entry points for
 * Spike$Fast.add(int,int) and Spike$Fast.count(), built as an Emscripten side
 * module and installed at runtime via WASM_JIT_SPIKE (see README.md).
 * Imports wasm_jit_sp/wasm_jit_set_sp from the main module (HotSpot).
 */
#include <stdint.h>

extern intptr_t* wasm_jit_sp(void* thread);
extern void wasm_jit_set_sp(void* thread, intptr_t* sp);

static int calls;

/* static int add(int a, int b): stack top is b, then a. */
int spike_add_entry(void* method, intptr_t base_pc, void* thread) {
  intptr_t* sp = wasm_jit_sp(thread);
  int b = (int) sp[0];
  int a = (int) sp[1];
  __atomic_fetch_add(&calls, 1, __ATOMIC_RELAXED);
  sp[1] = (intptr_t) (a + b);   /* result replaces the parameters */
  wasm_jit_set_sp(thread, sp + 1);
  return 0;                     /* no deoptimized frames */
}

/* static int count(): no parameters, push the result. */
int spike_count_entry(void* method, intptr_t base_pc, void* thread) {
  intptr_t* sp = wasm_jit_sp(thread) - 1;
  sp[0] = (intptr_t) __atomic_load_n(&calls, __ATOMIC_RELAXED);
  wasm_jit_set_sp(thread, sp);
  return 0;
}
