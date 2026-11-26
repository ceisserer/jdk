/*
 * Experimental (wasm32): install Wasm code loaded at runtime as a method's
 * Zero entry point. See wasm/spike/README.md.
 *
 * WASM_JIT_SPIKE="pkg.Class.method=/path/module.so:symbol;..." makes
 * Method::link_method dlopen() the side module and point the method's
 * interpreter entry at a ZeroEntry whose entry_point is the symbol. The
 * function has the Zero entry ABI:
 *   int entry(Method* method, intptr_t base_pc, JavaThread* thread)
 * It pops its parameters from the ZeroStack, pushes the result and returns
 * the number of deoptimized frames (0).
 */
#ifdef __EMSCRIPTEN__

#include "entry_zero.hpp"
#include "jvm_io.h"
#include "memory/resourceArea.hpp"
#include "oops/method.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "wasmJitSpike_zero.hpp"

#include <dlfcn.h>
#include <string.h>

// Runtime helpers imported by the side modules (resolved by the dynamic linker
// against the main module).
extern "C" JNIEXPORT intptr_t* wasm_jit_sp(JavaThread* thread) {
  return thread->zero_stack()->sp();
}
extern "C" JNIEXPORT void wasm_jit_set_sp(JavaThread* thread, intptr_t* sp) {
  thread->zero_stack()->set_sp(sp);
}

// dlopen() makes every thread load the module into its own Wasm instance. A
// thread only does that while it waits (futex) and processes its Emscripten
// mailbox, so a thread spinning in Java code would block dlopen forever. Load
// at a safepoint instead: all Java threads are then parked and waiting.
class VM_WasmDlopen : public VM_Operation {
  const char* _path;
  const char* _symbol;
  void* _fn;
 public:
  VM_WasmDlopen(const char* path, const char* symbol) : _path(path), _symbol(symbol), _fn(nullptr) {}
  VMOp_Type type() const { return VMOp_ForceSafepoint; }
  void doit() {
    void* handle = ::dlopen(_path, RTLD_NOW | RTLD_GLOBAL);
    if (handle != nullptr) {
      _fn = ::dlsym(handle, _symbol);
    } else {
      fprintf(stderr, "[wasm-jit-spike] dlopen(%s) failed: %s\n", _path, ::dlerror());
    }
  }
  void* fn() const { return _fn; }
};

void WasmJitSpike::maybe_install(const methodHandle& method, TRAPS) {
  const char* spec = ::getenv("WASM_JIT_SPIKE");
  if (spec == nullptr) {
    return;
  }
  ResourceMark rm(THREAD);
  char name[512];
  jio_snprintf(name, sizeof(name), "%s.%s",
               method->method_holder()->external_name(), method->name()->as_C_string());
  size_t name_len = strlen(name);

  // Find "<name>=<path>:<symbol>" in the ';'-separated spec.
  for (const char* p = spec; *p != '\0'; ) {
    const char* end = strchr(p, ';');
    size_t len = end != nullptr ? (size_t)(end - p) : strlen(p);
    if (len > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
      char entry[1024];
      jio_snprintf(entry, MIN2(sizeof(entry), len - name_len), "%s", p + name_len + 1);
      char* colon = strrchr(entry, ':');
      if (colon == nullptr) {
        fprintf(stderr, "[wasm-jit-spike] bad entry for %s\n", name);
        return;
      }
      *colon = '\0';
      const char* path = entry;
      const char* symbol = colon + 1;

      jlong start = os::javaTimeNanos();
      VM_WasmDlopen op(path, symbol);
      VMThread::execute(&op);
      void* fn = op.fn();
      if (fn == nullptr) {
        fprintf(stderr, "[wasm-jit-spike] symbol %s not found in %s\n", symbol, path);
        return;
      }
      ZeroEntry* zero_entry = (ZeroEntry*) os::malloc(sizeof(ZeroEntry), mtCode);
      zero_entry->set_entry_point((address) fn);
      method->set_interpreter_entry((address) zero_entry);
      fprintf(stderr, "[wasm-jit-spike] %s -> %s:%s (dlopen+dlsym %.2f ms, thread %s)\n",
              name, path, symbol, (os::javaTimeNanos() - start) / 1e6, THREAD->name());
      return;
    }
    if (end == nullptr) break;
    p = end + 1;
  }
}

#endif // __EMSCRIPTEN__
