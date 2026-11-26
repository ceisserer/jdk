/*
 * Experimental (wasm32): install Wasm code loaded at runtime with dlopen as a
 * method's Zero entry point. Only active when WASM_JIT_SPIKE is set; see
 * wasm/spike/README.md.
 */
#ifndef CPU_ZERO_WASMJITSPIKE_ZERO_HPP
#define CPU_ZERO_WASMJITSPIKE_ZERO_HPP

#include "memory/allStatic.hpp"
#include "runtime/handles.hpp"

class WasmJitSpike : AllStatic {
 public:
  // Called at the end of Method::link_method.
  static void maybe_install(const methodHandle& method, TRAPS);
};

#endif // CPU_ZERO_WASMJITSPIKE_ZERO_HPP
