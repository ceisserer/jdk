/*
 * Experimental baseline compiler for HotSpot Zero on wasm32: translates the
 * bytecode of hot methods into WebAssembly, loads it as an Emscripten side
 * module at a safepoint and installs it as the method's Zero entry point.
 *
 * Compiled code keeps a genuine interpreter frame (locals and expression stack
 * in the Zero stack, same layout as the interpreter), so GC, stack walking and
 * exceptions work unchanged. Anything not handled (unsupported bytecodes,
 * exceptional paths such as null or out-of-bounds, unresolved constant pool
 * entries) bails out: the frame is handed to the interpreter at the current
 * bytecode, which finishes the method.
 *
 * Loading a module synchronizes all threads (each instantiates it), so hot
 * methods are queued and compiled in batches, one module per batch, by a
 * periodic task on the WatcherThread (in the background; the interpreter keeps
 * running the methods meanwhile).
 *
 * Enabled with the environment variable WASM_JIT=1. WASM_JIT_THRESHOLD sets the
 * number of interpreted invocations before compiling (default 1000),
 * WASM_JIT_BATCH_MS the batching interval (default 50, multiple of 10; 0 =
 * compile synchronously in the calling thread, one method per module),
 * WASM_JIT_BATCH_MAX the maximum number of methods per module (default 256),
 * WASM_JIT_LOG=1 logs compilations, WASM_JIT_DIR the directory for the
 * generated modules (default /tmp).
 */
#ifndef CPU_ZERO_WASMJIT_ZERO_HPP
#define CPU_ZERO_WASMJIT_ZERO_HPP

#ifdef __EMSCRIPTEN__

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class BytecodeInterpreter;
class JavaThread;
class Method;

class WasmJit : AllStatic {
 public:
  static bool enabled() { return _enabled; }
  static bool log() { return _log; }
  // false until initialized (then the result of WASM_JIT)
  static bool disabled() { return _disabled; }
  static void initialize();
  // Called by ZeroInterpreter::normal_entry for every interpreted invocation.
  static void on_interpreted_invocation(Method* method, JavaThread* thread);
  // Called by the interpreter on backward branches: true if it should stop and
  // let main_loop try on-stack replacement (msg do_osr).
  static inline bool osr_check(Method* method);
  // Called by ZeroInterpreter::main_loop for do_osr: if the method has
  // compiled code, continue the current frame in it and return true (the
  // compiled code finished the method: frame popped, result pushed).
  // Otherwise queue it for compilation and return false.
  static bool osr(JavaThread* thread, BytecodeInterpreter* istate);
  // Compile and load the queued methods (WatcherThread).
  static void flush_queue();

  // Offsets into the interpreter state used by generated code.
  static int istate_locals_offset();
  static int istate_stack_limit_offset();

 private:
  static bool _enabled;
  static bool _disabled;
  static int _threshold;
  static int _osr_threshold;
  static bool _log;
  // Queue the method for compilation (or compile it right away if batching is
  // off). The thread is in Java state with the frame anchor cleared.
  static void request_compilation(Method* method, JavaThread* thread);
  static void enqueue(Method* method, JavaThread* thread);
};

#include "oops/method.hpp"
#include "oops/methodCounters.hpp"

inline bool WasmJit::osr_check(Method* method) {
  MethodCounters* mcs = method->method_counters();
  if (mcs == nullptr) return false;
  InvocationCounter* bc = mcs->backedge_counter();
  bc->increment();
  return (bc->count() % (uint) _osr_threshold) == 0;
}

#endif // __EMSCRIPTEN__
#endif // CPU_ZERO_WASMJIT_ZERO_HPP
