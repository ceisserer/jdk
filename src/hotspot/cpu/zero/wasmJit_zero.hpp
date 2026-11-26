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
 * Enabled with the environment variable WASM_JIT=1. WASM_JIT_THRESHOLD sets the
 * number of interpreted invocations before compiling (default 1000),
 * WASM_JIT_LOG=1 logs compilations, WASM_JIT_DIR the directory for the
 * generated modules (default /tmp).
 */
#ifndef CPU_ZERO_WASMJIT_ZERO_HPP
#define CPU_ZERO_WASMJIT_ZERO_HPP

#ifdef __EMSCRIPTEN__

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class JavaThread;
class Method;

class WasmJit : AllStatic {
 public:
  static bool enabled() { return _enabled; }
  // false until initialized (then the result of WASM_JIT)
  static bool disabled() { return _disabled; }
  static void initialize();
  // Called by ZeroInterpreter::normal_entry for every interpreted invocation.
  static void on_interpreted_invocation(Method* method, JavaThread* thread);

  // Offsets into the interpreter state used by generated code.
  static int istate_locals_offset();
  static int istate_stack_limit_offset();

 private:
  static bool _enabled;
  static bool _disabled;
  static int _threshold;
  static bool _log;
  static void compile_and_install(Method* method, JavaThread* thread);
};

#endif // __EMSCRIPTEN__
#endif // CPU_ZERO_WASMJIT_ZERO_HPP
