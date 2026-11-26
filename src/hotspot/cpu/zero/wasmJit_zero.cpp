/*
 * Experimental baseline compiler for HotSpot Zero on wasm32. See
 * wasmJit_zero.hpp for the design and the environment variables.
 */
#ifdef __EMSCRIPTEN__

#include "entry_zero.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/cardTable.hpp"
#include "gc/shared/cardTableBarrierSet.hpp"
#include "interpreter/bytecode.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/bytecodes.hpp"
#include "interpreter/zero/bytecodeInterpreter.hpp"
#include "interpreter/zero/zeroInterpreter.hpp"
#include "jvm_io.h"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/arrayOop.hpp"
#include "oops/klassVtable.hpp"
#include "oops/objArrayKlass.hpp"
#include "runtime/signature.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/cpCache.inline.hpp"
#include "oops/method.inline.hpp"
#include "oops/methodCounters.hpp"
#include "oops/resolvedFieldEntry.hpp"
#include "oops/resolvedMethodEntry.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/arguments.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "stack_zero.inline.hpp"
#include "wasmEmitter_zero.hpp"
#include "wasmJit_zero.hpp"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

bool WasmJit::_enabled = false;
bool WasmJit::_disabled = false;
int  WasmJit::_threshold = 1000;
int  WasmJit::_osr_threshold = 10000;
bool WasmJit::_log = false;
static int  wasmjit_state = 0;          // 0: not initialized, 1: disabled, 2: enabled
static const char* wasmjit_dir = "/tmp";
static volatile int wasmjit_module_counter = 0;
static int wasmjit_log_level = 0;           // WASM_JIT_LOG: 1 compilations/OSR, 2 also bailouts
static int wasmjit_max_bailouts = 10000;    // WASM_JIT_MAX_BAILOUTS: then revert to the interpreter

// The method's interpreter entry while it has Wasm code. Starts like a
// ZeroEntry (entry_point), so the interpreter can call it unchanged.
struct WasmJitEntry {
  address entry_point;
  address interp_entry;      // previous entry, restored on revert
  volatile int bailouts;
};

int WasmJit::istate_locals_offset()      { return in_bytes(byte_offset_of(BytecodeInterpreter, _locals)); }
int WasmJit::istate_stack_limit_offset() { return in_bytes(byte_offset_of(BytecodeInterpreter, _stack_limit)); }

void WasmJit::initialize() {
  const char* e = ::getenv("WASM_JIT");
  _enabled = e != nullptr && strcmp(e, "0") != 0;
  const char* t = ::getenv("WASM_JIT_THRESHOLD");
  if (t != nullptr) _threshold = MAX2(1, atoi(t));
  const char* o = ::getenv("WASM_JIT_OSR_THRESHOLD");
  if (o != nullptr) _osr_threshold = MAX2(1, atoi(o));
  const char* l = ::getenv("WASM_JIT_LOG");
  _log = l != nullptr && strcmp(l, "0") != 0;
  wasmjit_log_level = l != nullptr ? atoi(l) : 0;
  const char* mb = ::getenv("WASM_JIT_MAX_BAILOUTS");
  if (mb != nullptr) wasmjit_max_bailouts = atoi(mb);
  const char* d = ::getenv("WASM_JIT_DIR");
  if (d != nullptr) wasmjit_dir = d;
  wasmjit_state = _enabled ? 2 : 1;
  _disabled = !_enabled;
}

// ---------------------------------------------------------------------------
// Runtime helpers called from generated code (imported by name from the main
// module). They mirror ZeroInterpreter::normal_entry / main_loop.

static intptr_t wasmjit_narrow(BasicType type, jint result) {
  switch (type) {
    case T_BOOLEAN: return result & 1;
    case T_BYTE:    return (jbyte) result;
    case T_CHAR:    return (jchar) result;
    case T_SHORT:   return (jshort) result;
    default:        return result;
  }
}

// Build and push an interpreter frame for the method (args already on the
// Zero stack). Returns the interpreter state, or null with a pending exception.
extern "C" JNIEXPORT BytecodeInterpreter* wasmjit_enter(Method* method, JavaThread* thread) {
  InterpreterFrame* frame = InterpreterFrame::build(method, thread);
  if (thread->has_pending_exception()) {
    return nullptr;
  }
  thread->push_zero_frame(frame);
  thread->set_last_Java_frame();
  return frame->interpreter_state();
}

// Return from compiled code: pop the frame and push the result (slots words
// at the top of the expression stack; tos is the next free slot).
extern "C" JNIEXPORT void wasmjit_return(JavaThread* thread, BytecodeInterpreter* istate,
                                         intptr_t* tos, int slots) {
  Method* method = istate->method();
  ZeroInterpreter::stack_watermark_unwind_check(thread);
  thread->reset_last_Java_frame();
  intptr_t* result = tos + slots;
  thread->pop_zero_frame();
  ZeroStack* stack = thread->zero_stack();
  stack->set_sp(stack->sp() + method->max_locals());
  for (int i = 0; i < slots; i++) {
    intptr_t res = result[-i];
    if (slots == 1 && is_subword_type(method->result_type())) {
      res = wasmjit_narrow(method->result_type(), (jint) res);
    }
    stack->push(res);
  }
}

// Hand the frame to the interpreter at bci (re-executing that bytecode, or
// handling the pending exception there) and let it finish the method.
extern "C" JNIEXPORT void wasmjit_bailout(JavaThread* thread, BytecodeInterpreter* istate,
                                          int bci, intptr_t* tos, int with_exception) {
  istate->set_bcp(istate->method()->code_base() + bci);
  if (tos != nullptr) istate->set_stack(tos);
  if (wasmjit_log_level >= 2) {
    ResourceMark rm(thread);
    Method* m = istate->method();
    fprintf(stderr, "[wasmjit] bailout %s bci %d (%s)%s\n", m->name_and_sig_as_C_string(), bci,
            Bytecodes::name(Bytecodes::java_code(Bytecodes::code_at(m, m->code_base() + bci))),
            with_exception ? " with exception" : "");
  }
  Method* bm = istate->method();
  if (bm->is_not_c2_compilable() && wasmjit_max_bailouts > 0) {
    // Compiled code that keeps bailing out is slower than the interpreter.
    WasmJitEntry* e = (WasmJitEntry*) bm->from_interpreted_entry();
    if (AtomicAccess::add(&e->bailouts, 1) == wasmjit_max_bailouts) {
      bm->set_interpreter_entry(e->interp_entry);
      bm->clear_is_not_c2_compilable();
      if (wasmjit_log_level >= 1) {
        ResourceMark rm(thread);
        fprintf(stderr, "[wasmjit] reverted %s to the interpreter after %d bailouts\n",
                bm->name_and_sig_as_C_string(), wasmjit_max_bailouts);
      }
    }
  }
  istate->set_msg(with_exception ? BytecodeInterpreter::method_resume
                                 : BytecodeInterpreter::deopt_resume2);
  ZeroInterpreter::main_loop(0, thread);
}

JRT_ENTRY(static void, wasmjit_safepoint_vm(JavaThread* current)) {
  // Transition to VM and back processes the safepoint / handshake.
}
JRT_END

// Safepoint poll from a backward branch to bci. Returns 1 if an (async)
// exception is pending afterwards.
extern "C" JNIEXPORT int wasmjit_safepoint(JavaThread* thread, BytecodeInterpreter* istate,
                                           int bci, intptr_t* tos) {
  istate->set_bcp(istate->method()->code_base() + bci);
  istate->set_stack(tos);
  wasmjit_safepoint_vm(thread);
  return thread->has_pending_exception() ? 1 : 0;
}

// OSR entry: compiled code continues the interpreter frame on top of the stack.
extern "C" JNIEXPORT BytecodeInterpreter* wasmjit_osr_enter(JavaThread* thread) {
  thread->set_last_Java_frame();
  return thread->top_zero_frame()->as_interpreter_frame()->interpreter_state();
}

// Before a call: make the frame walkable at bci (expression stack including the
// arguments), put the Zero stack pointer at the arguments and clear the frame
// anchor (the callee sets its own), as ZeroInterpreter::main_loop does.
extern "C" JNIEXPORT void wasmjit_pre_call(JavaThread* thread, BytecodeInterpreter* istate,
                                           int bci, intptr_t* tos) {
  istate->set_bcp(istate->method()->code_base() + bci);
  istate->set_stack(tos);
  thread->zero_stack()->set_sp(tos + 1);
  thread->reset_last_Java_frame();
}

// After a call: restore the Zero stack pointer and the frame anchor. Returns 1
// if an exception is pending.
extern "C" JNIEXPORT int wasmjit_post_call(JavaThread* thread, BytecodeInterpreter* istate) {
  thread->zero_stack()->set_sp(istate->stack_limit() + 1);
  thread->set_last_Java_frame();
  return thread->has_pending_exception() ? 1 : 0;
}

// Allocation through InterpreterRuntime (kind 0: new, 1: newarray, 2: anewarray).
// Returns the object, or null with a pending exception.
extern "C" JNIEXPORT oopDesc* wasmjit_new(JavaThread* thread, BytecodeInterpreter* istate, int bci,
                                          intptr_t* tos, int kind, int index, int size) {
  istate->set_bcp(istate->method()->code_base() + bci);
  istate->set_stack(tos);
  ConstantPool* cp = istate->method()->constants();
  switch (kind) {
    case 0:  InterpreterRuntime::_new(thread, cp, index); break;
    case 1:  InterpreterRuntime::newarray(thread, (BasicType) index, size); break;
    default: InterpreterRuntime::anewarray(thread, cp, index, size); break;
  }
  if (thread->has_pending_exception()) {
    return nullptr;
  }
  oop result = thread->vm_result_oop();
  thread->set_vm_result_oop(nullptr);
  return result;
}

// _return_register_finalizer (the return of Object.<init>): register the
// receiver if its class has a finalizer. Returns 1 if an exception is pending.
extern "C" JNIEXPORT int wasmjit_register_finalizer(JavaThread* thread, BytecodeInterpreter* istate,
                                                    int bci, intptr_t* tos) {
  oop rcvr = cast_to_oop((void*) istate->locals()[0]);
  if (!rcvr->klass()->has_finalizer()) return 0;
  istate->set_bcp(istate->method()->code_base() + bci);
  istate->set_stack(tos);
  InterpreterRuntime::register_finalizer(thread, rcvr);
  return thread->has_pending_exception() ? 1 : 0;
}

extern "C" JNIEXPORT int wasmjit_subtype(Klass* sub, Klass* super) {
  return sub->is_subtype_of(super) ? 1 : 0;
}

// Target of an invokeinterface (mirrors BytecodeInterpreter). Returns null if
// the call cannot proceed (the interpreter then throws the right error).
extern "C" JNIEXPORT Method* wasmjit_interface_callee(oopDesc* rcvr, ResolvedMethodEntry* entry) {
  Klass* rk = rcvr->klass();
  if (entry->is_forced_virtual()) {
    return entry->is_vfinal() ? entry->method() : (Method*) rk->method_at_vtable(entry->table_index());
  }
  if (entry->is_vfinal()) {
    return rk->is_subtype_of(entry->interface_klass()) ? entry->method() : nullptr;
  }
  Method* interface_method = entry->method();
  InstanceKlass* iclass = interface_method->method_holder();
  if (!rk->is_instance_klass()) return nullptr;
  InstanceKlass* ik = InstanceKlass::cast(rk);
  Klass* refc = entry->interface_klass();
  itableOffsetEntry* scan = (itableOffsetEntry*) ik->start_of_itable();
  for (; scan->interface_klass() != nullptr; scan++) {
    if (scan->interface_klass() == refc) break;
  }
  if (scan->interface_klass() == nullptr) return nullptr;
  itableOffsetEntry* ki = (itableOffsetEntry*) ik->start_of_itable();
  int i;
  for (i = 0; i < ik->itable_length(); i++, ki++) {
    if (ki->interface_klass() == iclass) break;
  }
  if (i == ik->itable_length()) return nullptr;
  itableMethodEntry* im = ki->first_method_entry(rk);
  return im[interface_method->itable_index()].method();
}

// ---------------------------------------------------------------------------
// Compiler

class WasmJitCompiler : public StackObj {
 public:
  // imported helper functions (function indices) and their types
  enum { H_enter, H_return, H_bailout, H_safepoint, H_pre_call, H_post_call, H_new, H_subtype,
         H_icallee, H_osr_enter, H_finalizer, H_count };
  enum { T_entry, T_enter, T_return, T_bailout, T_safepoint, T_void_i32, T_pre_call, T_post_call,
         T_new, T_i32x2, T_i32x1, T_count };
  // wasm locals of the generated function
  enum { L_method = 0, L_basepc, L_thread,                 // params
         L_istate, L_lbase, L_slow, L_bb, L_i0, L_i1,       // i32
         L_l0, L_l1,                                        // i64
         L_f0, L_f1,                                        // f32
         L_d0, L_d1 };                                      // f64
  enum Flow { FALL, COND, JUMP, END };
  // extra branch targets of a switch (besides the default in out_target)
  int _switch_targets[257];
  int _nswitch;

 private:
  methodHandle _m;
  Method* _method;
  ConstantPool* _cp;
  ConstantPoolCache* _cache;
  int _code_size, _max_locals, _max_stack;
  int* _block_of;      // bci -> block index (leaders only), -1 otherwise
  int* _depth;         // bci -> expression stack depth at the bytecode, -1 unknown
  int* _block_start;   // block -> bci
  int* _block_depth;   // block -> entry depth, -1 unreachable
  int _nblocks;
  bool _emit;          // false: analysis pass
  WasmBuf* _c;         // function body being emitted
  int _cur;            // current block while emitting
  int _extra;          // structured-control nesting inside the current block
  // card table barrier for oop stores
  bool _card_barrier;
  intptr_t _card_base;
  int _card_shift;
  const char* _fail;
  int _bail_sites;

 public:
  int bail_sites() const { return _bail_sites; }
  int nblocks() const { return _nblocks; }
  const char* failure() const { return _fail; }

  WasmJitCompiler(const methodHandle& m) : _m(m), _method(m()), _emit(false), _c(nullptr),
                                           _cur(0), _extra(0), _nswitch(0), _fail(nullptr), _bail_sites(0) {
    _cp = _method->constants();
    _cache = _cp->cache();
    _code_size = _method->code_size();
    _max_locals = _method->max_locals();
    _max_stack = _method->max_stack();
    _block_of = NEW_RESOURCE_ARRAY(int, _code_size + 1);
    _depth = NEW_RESOURCE_ARRAY(int, _code_size + 1);
    for (int i = 0; i <= _code_size; i++) { _block_of[i] = -1; _depth[i] = -1; }
    _nblocks = 0;
    _block_start = nullptr;
    _block_depth = nullptr;
    BarrierSet* bs = BarrierSet::barrier_set();
    _card_barrier = bs->is_a(BarrierSet::CardTableBarrierSet);
    if (_card_barrier) {
      CardTable* ct = barrier_set_cast<CardTableBarrierSet>(bs)->card_table();
      _card_base = (intptr_t) ct->byte_map_base();
      _card_shift = CardTable::card_shift();
    }
  }

  // ---- low level emission helpers (no-ops in the analysis pass)
  void op(int o)               { if (_emit) _c->u8(o); }
  void lget(int l)             { if (_emit) { _c->u8(OP_local_get); _c->uleb(l); } }
  void lset(int l)             { if (_emit) { _c->u8(OP_local_set); _c->uleb(l); } }
  void ltee(int l)             { if (_emit) { _c->u8(OP_local_tee); _c->uleb(l); } }
  void i32c(int32_t v)         { if (_emit) { _c->u8(OP_i32_const); _c->sleb(v); } }
  void i64c(int64_t v)         { if (_emit) { _c->u8(OP_i64_const); _c->sleb(v); } }
  void f32c(float v)           { if (_emit) { _c->u8(OP_f32_const); _c->f32(v); } }
  void f64c(double v)          { if (_emit) { _c->u8(OP_f64_const); _c->f64(v); } }
  // memory access; the alignment hint must not exceed the access width (Zero
  // stack slots are only 4-byte aligned, so 8-byte accesses use 4 as well)
  void mem(int o, int off) {
    if (!_emit) return;
    int align = (o == OP_i32_load8_s || o == OP_i32_load8_u || o == OP_i32_store8) ? 0
              : (o == OP_i32_load16_s || o == OP_i32_load16_u || o == OP_i32_store16) ? 1 : 2;
    _c->u8(o); _c->uleb(align); _c->uleb(off);
  }
  void atomic(int sub, int align, int off) {
    if (_emit) { _c->u8(OP_prefix_fe); _c->uleb(sub); _c->uleb(align); _c->uleb(off); }
  }
  void fc(int sub)             { if (_emit) { _c->u8(OP_prefix_fc); _c->uleb(sub); } }
  void call(int f)             { if (_emit) { _c->u8(OP_call); _c->uleb(f); } }
  void if_()                   { if (_emit) { _c->u8(OP_if); _c->u8(0x40); } _extra++; }
  void if_i32()                { if (_emit) { _c->u8(OP_if); _c->u8(W_I32); } _extra++; }
  void if_i64()                { if (_emit) { _c->u8(OP_if); _c->u8(W_I64); } _extra++; }
  void else_()                 { op(OP_else); }
  void end()                   { op(OP_end); _extra--; }

  // ---- frame addressing
  // Expression stack slot d (0 = bottom) is at slow + soff(d); local i at
  // lbase + loff(i). A two-slot value is addressed by its higher slot index.
  int soff(int d) const { return 4 * (_max_stack - 1 - d); }
  int loff(int i) const { return 4 * (_max_locals - 1 - i); }
  void sld32(int d)  { lget(L_slow); mem(OP_i32_load, soff(d)); }
  void sld64(int d)  { lget(L_slow); mem(OP_i64_load, soff(d)); }
  void sldf(int d)   { lget(L_slow); mem(OP_f32_load, soff(d)); }
  void sldd(int d)   { lget(L_slow); mem(OP_f64_load, soff(d)); }
  void tos(int depth) { lget(L_slow); i32c(soff(depth)); op(OP_i32_add); }   // next free slot

  void bail(int bci, int depth, int with_exception) {
    if (_emit) _bail_sites++;
    lget(L_thread); lget(L_istate); i32c(bci); tos(depth); i32c(with_exception);
    call(H_bailout);
    i32c(0); op(OP_return);
  }
  // if (<i32 on stack>) bail
  void bail_if(int bci, int depth) { if_(); bail(bci, depth, 0); end(); }

  // ---- branches
  void poll(int target_bci, int target_depth) {
    lget(L_thread); mem(OP_i32_load, in_bytes(JavaThread::polling_word_offset()));
    i32c(SafepointMechanism::poll_bit()); op(OP_i32_and);
    if_();
      lget(L_thread); lget(L_istate); i32c(target_bci); tos(target_depth); call(H_safepoint);
      if_(); bail(target_bci, target_depth, 1); end();
    end();
  }
  void jump(int target_bci, int target_depth) {
    int j = _block_of[target_bci];
    if (j > _cur) {
      if (_emit) { _c->u8(OP_br); _c->uleb(j - (_cur + 1) + _extra); }
    } else {
      poll(target_bci, target_depth);
      i32c(target_bci); lset(L_bb);
      if (_emit) { _c->u8(OP_br); _c->uleb(_nblocks - 1 - _cur + _extra); }
    }
  }
  // condition (i32) is on the wasm stack
  void cond_jump(int target_bci, int target_depth) {
    int j = _block_of[target_bci];
    if (j > _cur) {
      if (_emit) { _c->u8(OP_br_if); _c->uleb(j - (_cur + 1) + _extra); }
    } else {
      if_(); jump(target_bci, target_depth); end();
    }
  }

  // ---- per-bytecode translation. Returns the flow kind; sets out_depth and
  // out_target. In the analysis pass only the effects are computed.
  Flow gen(BytecodeStream& bs, int D, int& out_depth, int& out_target);

  bool analyze();
  // Stack depth after a bytecode the compiler bails out at, so the code
  // after it stays compiled (reachable through branches and OSR); -1 if
  // unknown or if the bytecode does not fall through.
  int depth_after(BytecodeStream& bs, int D);
  bool emit_function(WasmBuf& body);
  bool compile(WasmBuf& module);
};

static int slots_of(TosState t) { return (t == ltos || t == dtos) ? 2 : 1; }

WasmJitCompiler::Flow WasmJitCompiler::gen(BytecodeStream& bs, int D, int& nd, int& target) {
  const Bytecodes::Code code = bs.code();
  const Bytecodes::Code raw = bs.raw_code();
  const int bci = bs.bci();
  const address bcp = bs.bcp();
  target = -1;
  nd = D;

#define BAIL() { bail(bci, D, 0); nd = depth_after(bs, D); return nd >= 0 ? FALL : END; }

  switch (code) {
    case Bytecodes::_nop: return FALL;
    case Bytecodes::_aconst_null: lget(L_slow); i32c(0); mem(OP_i32_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_iconst_m1: case Bytecodes::_iconst_0: case Bytecodes::_iconst_1: case Bytecodes::_iconst_2:
    case Bytecodes::_iconst_3: case Bytecodes::_iconst_4: case Bytecodes::_iconst_5:
      lget(L_slow); i32c((int) code - (int) Bytecodes::_iconst_0); mem(OP_i32_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_lconst_0: case Bytecodes::_lconst_1:
      lget(L_slow); i64c(code == Bytecodes::_lconst_1 ? 1 : 0); mem(OP_i64_store, soff(D + 1)); nd = D + 2; return FALL;
    case Bytecodes::_fconst_0: case Bytecodes::_fconst_1: case Bytecodes::_fconst_2:
      lget(L_slow); f32c((float) ((int) code - (int) Bytecodes::_fconst_0)); mem(OP_f32_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_dconst_0: case Bytecodes::_dconst_1:
      lget(L_slow); f64c(code == Bytecodes::_dconst_1 ? 1.0 : 0.0); mem(OP_f64_store, soff(D + 1)); nd = D + 2; return FALL;
    case Bytecodes::_bipush: lget(L_slow); i32c((jbyte) bcp[1]); mem(OP_i32_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_sipush: lget(L_slow); i32c((jshort) Bytes::get_Java_u2(bcp + 1)); mem(OP_i32_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_ldc: case Bytecodes::_ldc_w: case Bytecodes::_ldc2_w: {
      if (raw == Bytecodes::_fast_aldc || raw == Bytecodes::_fast_aldc_w) BAIL();
      int index = code == Bytecodes::_ldc ? bs.get_index() : bs.get_index_u2();
      constantTag tag = _cp->tag_at(index);
      if (tag.is_int())         { lget(L_slow); i32c(_cp->int_at(index));    mem(OP_i32_store, soff(D)); nd = D + 1; return FALL; }
      if (tag.is_float())       { lget(L_slow); f32c(_cp->float_at(index));  mem(OP_f32_store, soff(D)); nd = D + 1; return FALL; }
      if (tag.is_long())        { lget(L_slow); i64c(_cp->long_at(index));   mem(OP_i64_store, soff(D + 1)); nd = D + 2; return FALL; }
      if (tag.is_double())      { lget(L_slow); f64c(_cp->double_at(index)); mem(OP_f64_store, soff(D + 1)); nd = D + 2; return FALL; }
      BAIL();
    }

    // ---- locals
    case Bytecodes::_iload: case Bytecodes::_fload: case Bytecodes::_aload:
    case Bytecodes::_iload_0: case Bytecodes::_iload_1: case Bytecodes::_iload_2: case Bytecodes::_iload_3:
    case Bytecodes::_fload_0: case Bytecodes::_fload_1: case Bytecodes::_fload_2: case Bytecodes::_fload_3:
    case Bytecodes::_aload_0: case Bytecodes::_aload_1: case Bytecodes::_aload_2: case Bytecodes::_aload_3: {
      int n = (code == Bytecodes::_iload || code == Bytecodes::_fload || code == Bytecodes::_aload) ? bs.get_index()
            : (code >= Bytecodes::_aload_0) ? code - Bytecodes::_aload_0
            : (code >= Bytecodes::_fload_0) ? code - Bytecodes::_fload_0 : code - Bytecodes::_iload_0;
      lget(L_slow); lget(L_lbase); mem(OP_i32_load, loff(n)); mem(OP_i32_store, soff(D));
      nd = D + 1; return FALL;
    }
    case Bytecodes::_lload: case Bytecodes::_dload:
    case Bytecodes::_lload_0: case Bytecodes::_lload_1: case Bytecodes::_lload_2: case Bytecodes::_lload_3:
    case Bytecodes::_dload_0: case Bytecodes::_dload_1: case Bytecodes::_dload_2: case Bytecodes::_dload_3: {
      int n = (code == Bytecodes::_lload || code == Bytecodes::_dload) ? bs.get_index()
            : (code >= Bytecodes::_dload_0) ? code - Bytecodes::_dload_0 : code - Bytecodes::_lload_0;
      lget(L_slow); lget(L_lbase); mem(OP_i64_load, loff(n + 1)); mem(OP_i64_store, soff(D + 1));
      nd = D + 2; return FALL;
    }
    case Bytecodes::_istore: case Bytecodes::_fstore: case Bytecodes::_astore:
    case Bytecodes::_istore_0: case Bytecodes::_istore_1: case Bytecodes::_istore_2: case Bytecodes::_istore_3:
    case Bytecodes::_fstore_0: case Bytecodes::_fstore_1: case Bytecodes::_fstore_2: case Bytecodes::_fstore_3:
    case Bytecodes::_astore_0: case Bytecodes::_astore_1: case Bytecodes::_astore_2: case Bytecodes::_astore_3: {
      int n = (code == Bytecodes::_istore || code == Bytecodes::_fstore || code == Bytecodes::_astore) ? bs.get_index()
            : (code >= Bytecodes::_astore_0) ? code - Bytecodes::_astore_0
            : (code >= Bytecodes::_fstore_0) ? code - Bytecodes::_fstore_0 : code - Bytecodes::_istore_0;
      lget(L_lbase); sld32(D - 1); mem(OP_i32_store, loff(n));
      nd = D - 1; return FALL;
    }
    case Bytecodes::_lstore: case Bytecodes::_dstore:
    case Bytecodes::_lstore_0: case Bytecodes::_lstore_1: case Bytecodes::_lstore_2: case Bytecodes::_lstore_3:
    case Bytecodes::_dstore_0: case Bytecodes::_dstore_1: case Bytecodes::_dstore_2: case Bytecodes::_dstore_3: {
      int n = (code == Bytecodes::_lstore || code == Bytecodes::_dstore) ? bs.get_index()
            : (code >= Bytecodes::_dstore_0) ? code - Bytecodes::_dstore_0 : code - Bytecodes::_lstore_0;
      lget(L_lbase); sld64(D - 1); mem(OP_i64_store, loff(n + 1));
      nd = D - 2; return FALL;
    }
    case Bytecodes::_iinc: {
      int n, c;
      if (bs.is_wide()) { n = Bytes::get_Java_u2(bcp + 2); c = (jshort) Bytes::get_Java_u2(bcp + 4); }
      else              { n = bcp[1]; c = (jbyte) bcp[2]; }
      lget(L_lbase); lget(L_lbase); mem(OP_i32_load, loff(n)); i32c(c); op(OP_i32_add); mem(OP_i32_store, loff(n));
      return FALL;
    }

    // ---- stack manipulation (raw 32-bit words)
    case Bytecodes::_pop:  nd = D - 1; return FALL;
    case Bytecodes::_pop2: nd = D - 2; return FALL;
    case Bytecodes::_dup:
      lget(L_slow); sld32(D - 1); mem(OP_i32_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_dup_x1:   // ..b a -> ..a b a
      lget(L_slow); sld32(D - 1); mem(OP_i32_store, soff(D));
      lget(L_slow); sld32(D - 2); mem(OP_i32_store, soff(D - 1));
      lget(L_slow); sld32(D);     mem(OP_i32_store, soff(D - 2));
      nd = D + 1; return FALL;
    case Bytecodes::_dup_x2:   // ..c b a -> ..a c b a
      lget(L_slow); sld32(D - 1); mem(OP_i32_store, soff(D));
      lget(L_slow); sld32(D - 2); mem(OP_i32_store, soff(D - 1));
      lget(L_slow); sld32(D - 3); mem(OP_i32_store, soff(D - 2));
      lget(L_slow); sld32(D);     mem(OP_i32_store, soff(D - 3));
      nd = D + 1; return FALL;
    case Bytecodes::_dup2:     // ..b a -> ..b a b a
      lget(L_slow); sld32(D - 2); mem(OP_i32_store, soff(D));
      lget(L_slow); sld32(D - 1); mem(OP_i32_store, soff(D + 1));
      nd = D + 2; return FALL;
    case Bytecodes::_dup2_x1:  // ..c b a -> ..b a c b a
      lget(L_slow); sld32(D - 1); mem(OP_i32_store, soff(D + 1));
      lget(L_slow); sld32(D - 2); mem(OP_i32_store, soff(D));
      lget(L_slow); sld32(D - 3); mem(OP_i32_store, soff(D - 1));
      lget(L_slow); sld32(D + 1); mem(OP_i32_store, soff(D - 2));
      lget(L_slow); sld32(D);     mem(OP_i32_store, soff(D - 3));
      nd = D + 2; return FALL;
    case Bytecodes::_dup2_x2:  // ..d c b a -> ..b a d c b a
      lget(L_slow); sld32(D - 1); mem(OP_i32_store, soff(D + 1));
      lget(L_slow); sld32(D - 2); mem(OP_i32_store, soff(D));
      lget(L_slow); sld32(D - 3); mem(OP_i32_store, soff(D - 1));
      lget(L_slow); sld32(D - 4); mem(OP_i32_store, soff(D - 2));
      lget(L_slow); sld32(D + 1); mem(OP_i32_store, soff(D - 3));
      lget(L_slow); sld32(D);     mem(OP_i32_store, soff(D - 4));
      nd = D + 2; return FALL;
    case Bytecodes::_swap:
      sld32(D - 1); lset(L_i0);
      lget(L_slow); sld32(D - 2); mem(OP_i32_store, soff(D - 1));
      lget(L_slow); lget(L_i0);   mem(OP_i32_store, soff(D - 2));
      return FALL;

    // ---- int arithmetic
#define IBIN(bc, wop) case Bytecodes::bc: lget(L_slow); sld32(D - 2); sld32(D - 1); op(wop); mem(OP_i32_store, soff(D - 2)); nd = D - 1; return FALL;
    IBIN(_iadd, OP_i32_add) IBIN(_isub, OP_i32_sub) IBIN(_imul, OP_i32_mul)
    IBIN(_iand, OP_i32_and) IBIN(_ior, OP_i32_or)   IBIN(_ixor, OP_i32_xor)
    IBIN(_ishl, OP_i32_shl) IBIN(_ishr, OP_i32_shr_s) IBIN(_iushr, OP_i32_shr_u)
#undef IBIN
    case Bytecodes::_idiv: case Bytecodes::_irem:
      sld32(D - 1); op(OP_i32_eqz); bail_if(bci, D);          // ArithmeticException in the interpreter
      lget(L_slow);
      sld32(D - 1); i32c(-1); op(OP_i32_eq);
      if_i32();
        if (code == Bytecodes::_idiv) { i32c(0); sld32(D - 2); op(OP_i32_sub); } else { i32c(0); }
      else_();
        sld32(D - 2); sld32(D - 1); op(code == Bytecodes::_idiv ? OP_i32_div_s : OP_i32_rem_s);
      end();
      mem(OP_i32_store, soff(D - 2)); nd = D - 1; return FALL;
    case Bytecodes::_ineg: lget(L_slow); i32c(0); sld32(D - 1); op(OP_i32_sub); mem(OP_i32_store, soff(D - 1)); return FALL;

    // ---- long arithmetic: a at slot D-3, b at D-1
#define LBIN(bc, wop) case Bytecodes::bc: lget(L_slow); sld64(D - 3); sld64(D - 1); op(wop); mem(OP_i64_store, soff(D - 3)); nd = D - 2; return FALL;
    LBIN(_ladd, OP_i64_add) LBIN(_lsub, OP_i64_sub) LBIN(_lmul, OP_i64_mul)
    LBIN(_land, OP_i64_and) LBIN(_lor, OP_i64_or)   LBIN(_lxor, OP_i64_xor)
#undef LBIN
#define LSH(bc, wop) case Bytecodes::bc: lget(L_slow); sld64(D - 2); sld32(D - 1); op(OP_i64_extend_i32_u); op(wop); mem(OP_i64_store, soff(D - 2)); nd = D - 1; return FALL;
    LSH(_lshl, OP_i64_shl) LSH(_lshr, OP_i64_shr_s) LSH(_lushr, OP_i64_shr_u)
#undef LSH
    case Bytecodes::_ldiv: case Bytecodes::_lrem:
      sld64(D - 1); op(OP_i64_eqz); bail_if(bci, D);
      lget(L_slow);
      sld64(D - 1); i64c(-1); op(OP_i64_eq);
      if_i64();
        if (code == Bytecodes::_ldiv) { i64c(0); sld64(D - 3); op(OP_i64_sub); } else { i64c(0); }
      else_();
        sld64(D - 3); sld64(D - 1); op(code == Bytecodes::_ldiv ? OP_i64_div_s : OP_i64_rem_s);
      end();
      mem(OP_i64_store, soff(D - 3)); nd = D - 2; return FALL;
    case Bytecodes::_lneg: lget(L_slow); i64c(0); sld64(D - 1); op(OP_i64_sub); mem(OP_i64_store, soff(D - 1)); return FALL;

    // ---- float / double arithmetic (rem bails: no wasm instruction)
#define FBIN(bc, wop) case Bytecodes::bc: lget(L_slow); sldf(D - 2); sldf(D - 1); op(wop); mem(OP_f32_store, soff(D - 2)); nd = D - 1; return FALL;
    FBIN(_fadd, OP_f32_add) FBIN(_fsub, OP_f32_sub) FBIN(_fmul, OP_f32_mul) FBIN(_fdiv, OP_f32_div)
#undef FBIN
#define DBIN(bc, wop) case Bytecodes::bc: lget(L_slow); sldd(D - 3); sldd(D - 1); op(wop); mem(OP_f64_store, soff(D - 3)); nd = D - 2; return FALL;
    DBIN(_dadd, OP_f64_add) DBIN(_dsub, OP_f64_sub) DBIN(_dmul, OP_f64_mul) DBIN(_ddiv, OP_f64_div)
#undef DBIN
    case Bytecodes::_fneg: lget(L_slow); sldf(D - 1); op(OP_f32_neg); mem(OP_f32_store, soff(D - 1)); return FALL;
    case Bytecodes::_dneg: lget(L_slow); sldd(D - 1); op(OP_f64_neg); mem(OP_f64_store, soff(D - 1)); return FALL;

    // ---- conversions
    case Bytecodes::_i2l: lget(L_slow); sld32(D - 1); op(OP_i64_extend_i32_s); mem(OP_i64_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_i2f: lget(L_slow); sld32(D - 1); op(OP_f32_convert_i32_s); mem(OP_f32_store, soff(D - 1)); return FALL;
    case Bytecodes::_i2d: lget(L_slow); sld32(D - 1); op(OP_f64_convert_i32_s); mem(OP_f64_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_l2i: lget(L_slow); sld64(D - 1); op(OP_i32_wrap_i64); mem(OP_i32_store, soff(D - 2)); nd = D - 1; return FALL;
    case Bytecodes::_l2f: lget(L_slow); sld64(D - 1); op(OP_f32_convert_i64_s); mem(OP_f32_store, soff(D - 2)); nd = D - 1; return FALL;
    case Bytecodes::_l2d: lget(L_slow); sld64(D - 1); op(OP_f64_convert_i64_s); mem(OP_f64_store, soff(D - 1)); return FALL;
    case Bytecodes::_f2i: lget(L_slow); sldf(D - 1); fc(FC_i32_trunc_sat_f32_s); mem(OP_i32_store, soff(D - 1)); return FALL;
    case Bytecodes::_f2l: lget(L_slow); sldf(D - 1); fc(FC_i64_trunc_sat_f32_s); mem(OP_i64_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_f2d: lget(L_slow); sldf(D - 1); op(OP_f64_promote_f32); mem(OP_f64_store, soff(D)); nd = D + 1; return FALL;
    case Bytecodes::_d2i: lget(L_slow); sldd(D - 1); fc(FC_i32_trunc_sat_f64_s); mem(OP_i32_store, soff(D - 2)); nd = D - 1; return FALL;
    case Bytecodes::_d2l: lget(L_slow); sldd(D - 1); fc(FC_i64_trunc_sat_f64_s); mem(OP_i64_store, soff(D - 1)); return FALL;
    case Bytecodes::_d2f: lget(L_slow); sldd(D - 1); op(OP_f32_demote_f64); mem(OP_f32_store, soff(D - 2)); nd = D - 1; return FALL;
    case Bytecodes::_i2b: lget(L_slow); sld32(D - 1); op(OP_i32_extend8_s); mem(OP_i32_store, soff(D - 1)); return FALL;
    case Bytecodes::_i2s: lget(L_slow); sld32(D - 1); op(OP_i32_extend16_s); mem(OP_i32_store, soff(D - 1)); return FALL;
    case Bytecodes::_i2c: lget(L_slow); sld32(D - 1); i32c(0xffff); op(OP_i32_and); mem(OP_i32_store, soff(D - 1)); return FALL;

    // ---- comparisons
    case Bytecodes::_lcmp:
      sld64(D - 3); lset(L_l0); sld64(D - 1); lset(L_l1);
      lget(L_slow);
      lget(L_l0); lget(L_l1); op(OP_i64_gt_s); lget(L_l0); lget(L_l1); op(OP_i64_lt_s); op(OP_i32_sub);
      mem(OP_i32_store, soff(D - 4)); nd = D - 3; return FALL;
    case Bytecodes::_fcmpl: case Bytecodes::_fcmpg:
      sldf(D - 2); lset(L_f0); sldf(D - 1); lset(L_f1);
      lget(L_slow);
      lget(L_f0); lget(L_f1); op(OP_f32_gt); lget(L_f0); lget(L_f1); op(OP_f32_lt); op(OP_i32_sub);
      lget(L_f0); lget(L_f0); op(OP_f32_ne); lget(L_f1); lget(L_f1); op(OP_f32_ne); op(OP_i32_or);  // unordered
      op(code == Bytecodes::_fcmpl ? OP_i32_sub : OP_i32_add);
      mem(OP_i32_store, soff(D - 2)); nd = D - 1; return FALL;
    case Bytecodes::_dcmpl: case Bytecodes::_dcmpg:
      sldd(D - 3); lset(L_d0); sldd(D - 1); lset(L_d1);
      lget(L_slow);
      lget(L_d0); lget(L_d1); op(OP_f64_gt); lget(L_d0); lget(L_d1); op(OP_f64_lt); op(OP_i32_sub);
      lget(L_d0); lget(L_d0); op(OP_f64_ne); lget(L_d1); lget(L_d1); op(OP_f64_ne); op(OP_i32_or);
      op(code == Bytecodes::_dcmpl ? OP_i32_sub : OP_i32_add);
      mem(OP_i32_store, soff(D - 4)); nd = D - 3; return FALL;

    // ---- branches
#define IF0(bc, cmp) case Bytecodes::bc: target = bs.dest(); nd = D - 1; sld32(D - 1); cmp; cond_jump(target, nd); return COND;
    IF0(_ifeq, op(OP_i32_eqz)) IF0(_ifne, {}) IF0(_ifnull, op(OP_i32_eqz)) IF0(_ifnonnull, {})
    IF0(_iflt, { i32c(0); op(OP_i32_lt_s); }) IF0(_ifge, { i32c(0); op(OP_i32_ge_s); })
    IF0(_ifgt, { i32c(0); op(OP_i32_gt_s); }) IF0(_ifle, { i32c(0); op(OP_i32_le_s); })
#undef IF0
#define IF2(bc, wop) case Bytecodes::bc: target = bs.dest(); nd = D - 2; sld32(D - 2); sld32(D - 1); op(wop); cond_jump(target, nd); return COND;
    IF2(_if_icmpeq, OP_i32_eq) IF2(_if_icmpne, OP_i32_ne) IF2(_if_icmplt, OP_i32_lt_s)
    IF2(_if_icmpge, OP_i32_ge_s) IF2(_if_icmpgt, OP_i32_gt_s) IF2(_if_icmple, OP_i32_le_s)
    IF2(_if_acmpeq, OP_i32_eq) IF2(_if_acmpne, OP_i32_ne)
#undef IF2
    case Bytecodes::_goto:   target = bs.dest();   jump(target, D); return JUMP;
    case Bytecodes::_goto_w: target = bs.dest_w(); jump(target, D); return JUMP;

    // ---- returns
    case Bytecodes::_ireturn: case Bytecodes::_freturn: case Bytecodes::_areturn:
    case Bytecodes::_lreturn: case Bytecodes::_dreturn: case Bytecodes::_return: {
      if (raw == Bytecodes::_return_register_finalizer) {
        lget(L_thread); lget(L_istate); i32c(bci); tos(D); call(H_finalizer);
        if_(); bail(bci, D, 1); end();
      }
      int slots = (code == Bytecodes::_return) ? 0 : (code == Bytecodes::_lreturn || code == Bytecodes::_dreturn) ? 2 : 1;
      lget(L_thread); lget(L_istate); tos(D); i32c(slots); call(H_return);
      i32c(0); op(OP_return);
      return END;
    }

    // ---- arrays
    case Bytecodes::_arraylength:
      sld32(D - 1); op(OP_i32_eqz); bail_if(bci, D);
      lget(L_slow); sld32(D - 1); mem(OP_i32_load, arrayOopDesc::length_offset_in_bytes()); mem(OP_i32_store, soff(D - 1));
      return FALL;
    case Bytecodes::_iaload: case Bytecodes::_faload: case Bytecodes::_aaload: case Bytecodes::_baload:
    case Bytecodes::_caload: case Bytecodes::_saload: case Bytecodes::_laload: case Bytecodes::_daload: {
      BasicType t; int ld; int sz;
      switch (code) {
        case Bytecodes::_iaload: t = T_INT;    ld = OP_i32_load;     sz = 2; break;
        case Bytecodes::_faload: t = T_FLOAT;  ld = OP_i32_load;     sz = 2; break;
        case Bytecodes::_aaload: t = T_OBJECT; ld = OP_i32_load;     sz = 2; break;
        case Bytecodes::_baload: t = T_BYTE;   ld = OP_i32_load8_s;  sz = 0; break;
        case Bytecodes::_caload: t = T_CHAR;   ld = OP_i32_load16_u; sz = 1; break;
        case Bytecodes::_saload: t = T_SHORT;  ld = OP_i32_load16_s; sz = 1; break;
        case Bytecodes::_laload: t = T_LONG;   ld = OP_i64_load;     sz = 3; break;
        default:                 t = T_DOUBLE; ld = OP_i64_load;     sz = 3; break;
      }
      sld32(D - 2); op(OP_i32_eqz); bail_if(bci, D);                                   // null
      sld32(D - 1); sld32(D - 2); mem(OP_i32_load, arrayOopDesc::length_offset_in_bytes());
      op(OP_i32_ge_u); bail_if(bci, D);                                                // bounds
      lget(L_slow);
      sld32(D - 2); sld32(D - 1); i32c(sz); op(OP_i32_shl); op(OP_i32_add);
      mem(ld, arrayOopDesc::base_offset_in_bytes(t));
      if (sz == 3) { mem(OP_i64_store, soff(D - 1)); }                                // two slots replace arr, index
      else         { mem(OP_i32_store, soff(D - 2)); nd = D - 1; }
      return FALL;
    }
    case Bytecodes::_aastore: {
      if (!_card_barrier || Arguments::is_valhalla_enabled()) BAIL();
      int a = D - 3, ix = D - 2, v = D - 1;
      sld32(a); op(OP_i32_eqz); bail_if(bci, D);
      sld32(ix); sld32(a); mem(OP_i32_load, arrayOopDesc::length_offset_in_bytes());
      op(OP_i32_ge_u); bail_if(bci, D);
      sld32(v);
      if_();   // non-null value: check assignability (ArrayStoreException in the interpreter)
        sld32(v); mem(OP_i32_load, oopDesc::klass_offset_in_bytes()); ltee(L_i0);
        sld32(a); mem(OP_i32_load, oopDesc::klass_offset_in_bytes()); mem(OP_i32_load, in_bytes(ObjArrayKlass::element_klass_offset()));
        ltee(L_i1); op(OP_i32_ne);
        if_();
          lget(L_i0); lget(L_i1); call(H_subtype); op(OP_i32_eqz); bail_if(bci, D);
        end();
      end();
      sld32(a); sld32(ix); i32c(2); op(OP_i32_shl); op(OP_i32_add);
      i32c(arrayOopDesc::base_offset_in_bytes(T_OBJECT)); op(OP_i32_add); ltee(L_i0);
      sld32(v); mem(OP_i32_store, 0);
      lget(L_i0); i32c(_card_shift); op(OP_i32_shr_u); i32c((int) _card_base); op(OP_i32_add);
      i32c(CardTable::dirty_card_val()); mem(OP_i32_store8, 0);
      nd = D - 3; return FALL;
    }
    case Bytecodes::_iastore: case Bytecodes::_fastore: case Bytecodes::_bastore:
    case Bytecodes::_castore: case Bytecodes::_sastore: case Bytecodes::_lastore: case Bytecodes::_dastore: {
      bool wide = code == Bytecodes::_lastore || code == Bytecodes::_dastore;
      int a = wide ? D - 4 : D - 3, ix = wide ? D - 3 : D - 2;   // value at D-1
      BasicType t; int st; int sz;
      switch (code) {
        case Bytecodes::_iastore: t = T_INT;   st = OP_i32_store;   sz = 2; break;
        case Bytecodes::_fastore: t = T_FLOAT; st = OP_i32_store;   sz = 2; break;
        case Bytecodes::_bastore: t = T_BYTE;  st = OP_i32_store8;  sz = 0; break;
        case Bytecodes::_castore: t = T_CHAR;  st = OP_i32_store16; sz = 1; break;
        case Bytecodes::_sastore: t = T_SHORT; st = OP_i32_store16; sz = 1; break;
        case Bytecodes::_lastore: t = T_LONG;  st = OP_i64_store;   sz = 3; break;
        default:                  t = T_DOUBLE; st = OP_i64_store;  sz = 3; break;
      }
      sld32(a); op(OP_i32_eqz); bail_if(bci, D);
      sld32(ix); sld32(a); mem(OP_i32_load, arrayOopDesc::length_offset_in_bytes());
      op(OP_i32_ge_u); bail_if(bci, D);
      sld32(a); sld32(ix); i32c(sz); op(OP_i32_shl); op(OP_i32_add);
      if (wide) {
        sld64(D - 1);
      } else if (code == Bytecodes::_bastore) {
        // boolean[] stores are masked to 0/1 (byte[] and boolean[] share the bytecode)
        sld32(D - 1); i32c(1); i32c(0xff);
        sld32(a); mem(OP_i32_load, oopDesc::klass_offset_in_bytes());
        i32c((int)(intptr_t) Universe::boolArrayKlass()); op(OP_i32_eq);
        op(OP_select); op(OP_i32_and);
      } else {
        sld32(D - 1);
      }
      mem(st, arrayOopDesc::base_offset_in_bytes(t));
      nd = wide ? D - 4 : D - 3; return FALL;
    }

    // ---- fields (resolved at compile time; otherwise bail)
    case Bytecodes::_getfield: case Bytecodes::_getstatic:
    case Bytecodes::_putfield: case Bytecodes::_putstatic: {
      int index = Bytes::get_native_u2(bcp + 1);
      ResolvedFieldEntry* e = _cache->resolved_field_entry_at(index);
      if (!e->is_resolved(code)) BAIL();
      bool is_static = code == Bytecodes::_getstatic || code == Bytecodes::_putstatic;
      if (is_static && !e->field_holder()->is_initialized()) BAIL();
      if (e->is_flat()) BAIL();
      TosState ts = (TosState) e->tos_state();
      int n = slots_of(ts);
      int off = e->field_offset();
      bool vol = e->is_volatile();
      if (vol && !(ts == itos || ts == atos || ts == ltos)) BAIL();
      bool is_get = code == Bytecodes::_getfield || code == Bytecodes::_getstatic;
      if (!is_get && ts == atos && !_card_barrier) BAIL();
      int obj_slot = is_get ? D - 1 : D - 1 - n;   // getfield: object on top; putfield: below the value
      // object (or class mirror) -> wasm stack
      auto push_obj = [&]() {
        if (is_static) {
          i32c((int)(intptr_t) e->field_holder()->java_mirror_handle().ptr_raw());
          mem(OP_i32_load, 0);
        } else {
          sld32(obj_slot);
        }
      };
      if (!is_static) { sld32(obj_slot); op(OP_i32_eqz); bail_if(bci, D); }
      if (is_get) {
        int res = is_static ? D : D - 1;            // first result slot
        lget(L_slow);
        push_obj();
        switch (ts) {
          case btos: case ztos: mem(OP_i32_load8_s, off); break;
          case ctos: mem(OP_i32_load16_u, off); break;
          case stos: mem(OP_i32_load16_s, off); break;
          case itos: case ftos: case atos:
            if (vol) atomic(FE_i32_atomic_load, 2, off); else mem(OP_i32_load, off);
            break;
          case ltos: case dtos:
            if (vol) atomic(FE_i64_atomic_load, 3, off); else mem(OP_i64_load, off);
            break;
          default: ShouldNotReachHere();
        }
        mem(n == 2 ? OP_i64_store : OP_i32_store, soff(res + n - 1));
        nd = res + n;
      } else {
        if (ts == atos) {
          push_obj(); i32c(off); op(OP_i32_add); ltee(L_i0);
          sld32(D - 1);
          if (vol) atomic(FE_i32_atomic_store, 2, 0); else mem(OP_i32_store, 0);
          lget(L_i0); i32c(_card_shift); op(OP_i32_shr_u); i32c((int) _card_base); op(OP_i32_add);
          i32c(CardTable::dirty_card_val()); mem(OP_i32_store8, 0);
        } else {
          push_obj();
          if (n == 2) sld64(D - 1); else sld32(D - 1);
          if (ts == ztos) { i32c(1); op(OP_i32_and); }
          switch (ts) {
            case btos: case ztos: mem(OP_i32_store8, off); break;
            case ctos: case stos: mem(OP_i32_store16, off); break;
            case itos: case ftos:
              if (vol) atomic(FE_i32_atomic_store, 2, off); else mem(OP_i32_store, off);
              break;
            case ltos: case dtos:
              if (vol) atomic(FE_i64_atomic_store, 3, off); else mem(OP_i64_store, off);
              break;
            default: ShouldNotReachHere();
          }
        }
        nd = is_static ? D - n : D - n - 1;
      }
      return FALL;
    }

    // ---- calls
    case Bytecodes::_invokestatic: case Bytecodes::_invokespecial:
    case Bytecodes::_invokevirtual: case Bytecodes::_invokeinterface: {
      if (raw == Bytecodes::_invokehandle) BAIL();
      int index = Bytes::get_native_u2(bcp + 1);
      ResolvedMethodEntry* e = _cache->resolved_method_entry_at(index);
      if (!e->is_resolved(code) || e->has_appendix()) BAIL();
      int P = e->number_of_parameters();
      TosState ts = (TosState) e->tos_state();
      int R = ts == vtos ? 0 : slots_of(ts);
      int rslot = D - P;
      if (code != Bytecodes::_invokestatic) { sld32(rslot); op(OP_i32_eqz); bail_if(bci, D); }
      if (code == Bytecodes::_invokeinterface) {
        sld32(rslot); i32c((int)(intptr_t) e); call(H_icallee); ltee(L_i0);
        op(OP_i32_eqz); bail_if(bci, D);
      } else if (code == Bytecodes::_invokevirtual && !e->is_vfinal()) {
        sld32(rslot); mem(OP_i32_load, oopDesc::klass_offset_in_bytes());
        mem(OP_i32_load, in_bytes(Klass::vtable_start_offset()) + e->table_index() * vtableEntry::size_in_bytes()
                         + in_bytes(vtableEntry::method_offset()));
        lset(L_i0);
      } else {
        i32c((int)(intptr_t) e->method()); lset(L_i0);
      }
      lget(L_thread); lget(L_istate); i32c(bci); tos(D); call(H_pre_call);
      // callee->_from_interpreted_entry is a ZeroEntry; its first word is the entry function
      lget(L_i0);
      lget(L_i0); mem(OP_i32_load, in_bytes(Method::from_interpreted_offset())); ltee(L_i1);
      lget(L_thread);
      lget(L_i1); mem(OP_i32_load, 0);
      if (_emit) { _c->u8(OP_call_indirect); _c->uleb(T_entry); _c->uleb(0); }
      op(OP_drop);
      lget(L_thread); lget(L_istate); call(H_post_call);
      if_(); bail(bci, D - P, 1); end();
      nd = D - P + R;
      return FALL;
    }

    // ---- allocation
    case Bytecodes::_new: case Bytecodes::_newarray: case Bytecodes::_anewarray: {
      int kind = code == Bytecodes::_new ? 0 : code == Bytecodes::_newarray ? 1 : 2;
      int index = code == Bytecodes::_newarray ? bcp[1] : Bytes::get_Java_u2(bcp + 1);
      lget(L_thread); lget(L_istate); i32c(bci); tos(D); i32c(kind); i32c(index);
      if (kind == 0) i32c(0); else sld32(D - 1);
      call(H_new); ltee(L_i0);
      op(OP_i32_eqz); if_(); bail(bci, D, 1); end();
      int res = kind == 0 ? D : D - 1;
      lget(L_slow); lget(L_i0); mem(OP_i32_store, soff(res));
      nd = res + 1;
      return FALL;
    }

    // ---- type checks against a resolved class
    case Bytecodes::_checkcast: case Bytecodes::_instanceof: {
      int index = Bytes::get_Java_u2(bcp + 1);
      if (!_cp->tag_at(index).is_klass()) BAIL();
      int k = (int)(intptr_t) _cp->resolved_klass_at(index);
      if (code == Bytecodes::_checkcast) {
        sld32(D - 1);
        if_();
          sld32(D - 1); mem(OP_i32_load, oopDesc::klass_offset_in_bytes()); ltee(L_i0); i32c(k); op(OP_i32_ne);
          if_();
            lget(L_i0); i32c(k); call(H_subtype); op(OP_i32_eqz); bail_if(bci, D);   // ClassCastException
          end();
        end();
      } else {
        sld32(D - 1); lset(L_i1);
        lget(L_slow);
        lget(L_i1); op(OP_i32_eqz);
        if_i32();
          i32c(0);
        else_();
          lget(L_i1); mem(OP_i32_load, oopDesc::klass_offset_in_bytes()); ltee(L_i0); i32c(k); op(OP_i32_eq);
          if_i32(); i32c(1); else_(); lget(L_i0); i32c(k); call(H_subtype); end();
        end();
        mem(OP_i32_store, soff(D - 1));
      }
      return FALL;
    }

    // ---- switches: compare chain (keys and targets are known at compile time)
    case Bytecodes::_tableswitch: case Bytecodes::_lookupswitch: {
      int n, def;
      int keys[256], dests[256];
      if (code == Bytecodes::_tableswitch) {
        Bytecode_tableswitch sw(_method, bcp);
        n = sw.length();
        if (n > 256) BAIL();
        for (int i = 0; i < n; i++) { keys[i] = sw.low_key() + i; dests[i] = bci + sw.dest_offset_at(i); }
        def = bci + sw.default_offset();
      } else {
        Bytecode_lookupswitch sw(_method, bcp);
        n = sw.number_of_pairs();
        if (n > 256) BAIL();
        for (int i = 0; i < n; i++) { LookupswitchPair p = sw.pair_at(i); keys[i] = p.match(); dests[i] = bci + p.offset(); }
        def = bci + sw.default_offset();
      }
      nd = D - 1;
      _nswitch = n;
      for (int i = 0; i < n; i++) {
        _switch_targets[i] = dests[i];
        sld32(D - 1); i32c(keys[i]); op(OP_i32_eq); cond_jump(dests[i], nd);
      }
      target = def;
      jump(def, nd);
      return JUMP;
    }

    default:
      BAIL();
  }
#undef BAIL
}

int WasmJitCompiler::depth_after(BytecodeStream& bs, int D) {
  Bytecodes::Code code = bs.code();
  address bcp = bs.bcp();
  switch (code) {
    case Bytecodes::_getfield: case Bytecodes::_putfield:
    case Bytecodes::_getstatic: case Bytecodes::_putstatic: {
      ResolvedFieldEntry* e = _cache->resolved_field_entry_at(Bytes::get_native_u2(bcp + 1));
      Symbol* sig = _cp->uncached_signature_ref_at(e->constant_pool_index());
      int n = type2size[Signature::basic_type(sig)];
      switch (code) {
        case Bytecodes::_getfield:  return D - 1 + n;
        case Bytecodes::_getstatic: return D + n;
        case Bytecodes::_putfield:  return D - 1 - n;
        default:                    return D - n;
      }
    }
    case Bytecodes::_invokestatic: case Bytecodes::_invokespecial:
    case Bytecodes::_invokevirtual: case Bytecodes::_invokeinterface: {
      ResolvedMethodEntry* e = _cache->resolved_method_entry_at(Bytes::get_native_u2(bcp + 1));
      Symbol* sig = _cp->uncached_signature_ref_at(e->constant_pool_index());
      ArgumentSizeComputer asc(sig);
      int P = asc.size() + (code == Bytecodes::_invokestatic ? 0 : 1);
      return D - P + type2size[asc.return_type()];
    }
    case Bytecodes::_multianewarray: return D - bcp[3] + 1;
    case Bytecodes::_ldc: case Bytecodes::_ldc_w: return D + 1;
    case Bytecodes::_ldc2_w: return D + 2;
    case Bytecodes::_invokedynamic: case Bytecodes::_athrow: case Bytecodes::_jsr: case Bytecodes::_jsr_w:
    case Bytecodes::_ret: case Bytecodes::_ireturn: case Bytecodes::_lreturn: case Bytecodes::_freturn:
    case Bytecodes::_dreturn: case Bytecodes::_areturn: case Bytecodes::_return:
    case Bytecodes::_tableswitch: case Bytecodes::_lookupswitch:
      return -1;
    default: {
      int d = Bytecodes::depth(code);
      int r = D + d;
      return (r >= 0 && r <= _max_stack) ? r : -1;
    }
  }
}

bool WasmJitCompiler::analyze() {
  // 1. basic block leaders
  bool* leader = NEW_RESOURCE_ARRAY(bool, _code_size + 1);
  for (int i = 0; i <= _code_size; i++) leader[i] = false;
  leader[0] = true;
  {
    BytecodeStream bs(_m);
    Bytecodes::Code c;
    while ((c = bs.next()) >= 0) {
      int next = bs.next_bci();
      switch (c) {
        case Bytecodes::_ifeq: case Bytecodes::_ifne: case Bytecodes::_iflt: case Bytecodes::_ifge:
        case Bytecodes::_ifgt: case Bytecodes::_ifle: case Bytecodes::_if_icmpeq: case Bytecodes::_if_icmpne:
        case Bytecodes::_if_icmplt: case Bytecodes::_if_icmpge: case Bytecodes::_if_icmpgt: case Bytecodes::_if_icmple:
        case Bytecodes::_if_acmpeq: case Bytecodes::_if_acmpne: case Bytecodes::_ifnull: case Bytecodes::_ifnonnull:
        case Bytecodes::_goto:
          leader[bs.dest()] = true; leader[next] = true; break;
        case Bytecodes::_goto_w:
          leader[bs.dest_w()] = true; leader[next] = true; break;
        case Bytecodes::_tableswitch: {
          Bytecode_tableswitch sw(_method, bs.bcp());
          for (int i = 0; i < sw.length(); i++) leader[bs.bci() + sw.dest_offset_at(i)] = true;
          leader[bs.bci() + sw.default_offset()] = true; leader[next] = true; break;
        }
        case Bytecodes::_lookupswitch: {
          Bytecode_lookupswitch sw(_method, bs.bcp());
          for (int i = 0; i < sw.number_of_pairs(); i++) leader[bs.bci() + sw.pair_at(i).offset()] = true;
          leader[bs.bci() + sw.default_offset()] = true; leader[next] = true; break;
        }
        case Bytecodes::_jsr: case Bytecodes::_jsr_w: case Bytecodes::_ret:
        case Bytecodes::_ireturn: case Bytecodes::_lreturn: case Bytecodes::_freturn:
        case Bytecodes::_dreturn: case Bytecodes::_areturn: case Bytecodes::_return:
        case Bytecodes::_athrow:
          leader[next] = true; break;
        default: break;
      }
    }
  }
  _block_start = NEW_RESOURCE_ARRAY(int, _code_size + 1);
  for (int i = 0; i < _code_size; i++) {
    if (leader[i]) { _block_of[i] = _nblocks; _block_start[_nblocks++] = i; }
  }
  _block_start[_nblocks] = _code_size;
  _block_depth = NEW_RESOURCE_ARRAY(int, _nblocks);
  for (int b = 0; b < _nblocks; b++) _block_depth[b] = -1;

  // 2. expression stack depth at every reachable bytecode (worklist)
  int* work = NEW_RESOURCE_ARRAY(int, _nblocks * 2 + 4);   // each block is pushed at most once
  int nwork = 0;
  _block_depth[0] = 0;
  work[nwork++] = 0;
  _emit = false;
  while (nwork > 0) {
    int b = work[--nwork];
    int D = _block_depth[b];
    BytecodeStream bs(_m);
    bs.set_interval(_block_start[b], _block_start[b + 1]);
    Bytecodes::Code c;
    Flow f = FALL;
    while ((c = bs.next()) >= 0) {
      _depth[bs.bci()] = D;
      int nd, target;
      _nswitch = 0;
      f = gen(bs, D, nd, target);
      for (int i = 0; i < _nswitch; i++) {
        int tb = _block_of[_switch_targets[i]];
        if (_block_depth[tb] < 0) { _block_depth[tb] = nd; work[nwork++] = tb; }
        else if (_block_depth[tb] != nd) { _fail = "inconsistent stack depth"; return false; }
      }
      if (f == COND || f == JUMP) {
        int tb = _block_of[target];
        if (_block_depth[tb] < 0) { _block_depth[tb] = nd; work[nwork++] = tb; }
        else if (_block_depth[tb] != nd) { _fail = "inconsistent stack depth"; return false; }
      }
      if (f == JUMP || f == END) break;
      D = nd;
    }
    if (f == FALL || f == COND) {
      int nb = b + 1;
      if (nb < _nblocks) {
        if (_block_depth[nb] < 0) { _block_depth[nb] = D; work[nwork++] = nb; }
        else if (_block_depth[nb] != D) { _fail = "inconsistent stack depth"; return false; }
      }
    }
    if (nwork > _nblocks * 2) { _fail = "worklist overflow"; return false; }
  }
  return true;
}

bool WasmJitCompiler::emit_function(WasmBuf& body) {
  WasmBuf code;
  _c = &code;
  _emit = true;
  _extra = 0;
  // prologue. base_pc < 0: OSR, continue the interpreter frame on top of the
  // stack at bci = -(base_pc + 1); otherwise build a new frame.
  lget(L_basepc); i32c(0); op(OP_i32_lt_s);
  if_();
    lget(L_thread); call(H_osr_enter); lset(L_istate);
    lget(L_basepc); i32c(-1); op(OP_i32_xor); lset(L_bb);
  else_();
    lget(L_method); lget(L_thread); call(H_enter); ltee(L_istate);
    op(OP_i32_eqz); if_(); i32c(0); op(OP_return); end();
    i32c(0); lset(L_bb);
  end();
  lget(L_istate); mem(OP_i32_load, WasmJit::istate_locals_offset()); i32c(4 * (_max_locals - 1)); op(OP_i32_sub); lset(L_lbase);
  lget(L_istate); mem(OP_i32_load, WasmJit::istate_stack_limit_offset()); i32c(4); op(OP_i32_add); lset(L_slow);
  // dispatch on bci: loop { block_{n-1} ... block_0 { br_table } code_0 } code_1 ... code_{n-1} }
  op(OP_loop); code.u8(0x40);
  for (int b = 0; b < _nblocks; b++) { op(OP_block); code.u8(0x40); }
  lget(L_bb);
  op(OP_br_table); code.uleb(_code_size);
  for (int bci = 0; bci < _code_size; bci++) code.uleb(_block_of[bci] >= 0 ? _block_of[bci] : 0);
  code.uleb(0);
  for (int b = 0; b < _nblocks; b++) {
    op(OP_end);                       // end of block_b: code of block b follows
    _cur = b;
    _extra = 0;
    int D = _block_depth[b];
    if (D < 0) {
      // Not reachable from the method entry in compiled code (depth unknown),
      // but an OSR may land here: return to the interpreter, keeping its tos.
      lget(L_thread); lget(L_istate); i32c(_block_start[b]); i32c(0); i32c(0); call(H_bailout);
      i32c(0); op(OP_return);
      continue;
    }
    BytecodeStream bs(_m);
    bs.set_interval(_block_start[b], _block_start[b + 1]);
    Bytecodes::Code c;
    while ((c = bs.next()) >= 0) {
      int nd, target;
      Flow f = gen(bs, D, nd, target);
      assert(_extra == 0, "unbalanced control structures");
      if (f == JUMP || f == END) break;
      D = nd;
    }
  }
  op(OP_end);                         // loop
  op(OP_unreachable);
  op(OP_end);                         // function

  // locals: 6 x i32, 2 x i64, 2 x f32, 2 x f64
  body.uleb(4);
  body.uleb(6); body.u8(W_I32);
  body.uleb(2); body.u8(W_I64);
  body.uleb(2); body.u8(W_F32);
  body.uleb(2); body.u8(W_F64);
  body.append(code);
  _c = nullptr;
  return true;
}

bool WasmJitCompiler::compile(WasmBuf& module) {
  if (!analyze()) return false;
  WasmBuf body;
  if (!emit_function(body)) return false;

  module.u8(0x00); module.u8('a'); module.u8('s'); module.u8('m');
  module.u8(1); module.u8(0); module.u8(0); module.u8(0);
  { // dylink.0: no memory, no table entries of our own
    WasmBuf s; s.str("dylink.0");
    s.u8(1); s.uleb(4); s.uleb(0); s.uleb(0); s.uleb(0); s.uleb(0);   // WASM_DYLINK_MEM_INFO
    module.section(0, s);
  }
  { // types
    WasmBuf s; s.uleb(T_count);
    auto fn = [&](int np, int nr) { s.u8(0x60); s.uleb(np); for (int i = 0; i < np; i++) s.u8(W_I32);
                                    s.uleb(nr); for (int i = 0; i < nr; i++) s.u8(W_I32); };
    fn(3, 1);   // T_entry     (method, base_pc, thread) -> int
    fn(2, 1);   // T_enter     (method, thread) -> istate
    fn(4, 0);   // T_return    (thread, istate, tos, slots)
    fn(5, 0);   // T_bailout   (thread, istate, bci, tos, with_exception)
    fn(4, 1);   // T_safepoint (thread, istate, bci, tos) -> pending exception
    fn(0, 1);   // T_void_i32  _emscripten_tls_init
    fn(4, 0);   // T_pre_call  (thread, istate, bci, tos)
    fn(2, 1);   // T_post_call (thread, istate) -> pending exception
    fn(7, 1);   // T_new       (thread, istate, bci, tos, kind, index, size) -> oop
    fn(2, 1);   // T_i32x2     subtype(sub, super), interface_callee(rcvr, entry)
    fn(1, 1);   // T_i32x1     osr_enter(thread) -> istate
    module.section(1, s);
  }
  { // imports
    WasmBuf s; s.uleb(2 + H_count);
    s.str("env"); s.str("memory"); s.u8(2); s.u8(0x03); s.uleb(1); s.uleb(65536);   // shared memory
    s.str("env"); s.str("__indirect_function_table"); s.u8(1); s.u8(0x70); s.u8(0x00); s.uleb(0);  // for call_indirect
    s.str("env"); s.str("wasmjit_enter");     s.u8(0); s.uleb(T_enter);
    s.str("env"); s.str("wasmjit_return");    s.u8(0); s.uleb(T_return);
    s.str("env"); s.str("wasmjit_bailout");   s.u8(0); s.uleb(T_bailout);
    s.str("env"); s.str("wasmjit_safepoint"); s.u8(0); s.uleb(T_safepoint);
    s.str("env"); s.str("wasmjit_pre_call");  s.u8(0); s.uleb(T_pre_call);
    s.str("env"); s.str("wasmjit_post_call"); s.u8(0); s.uleb(T_post_call);
    s.str("env"); s.str("wasmjit_new");       s.u8(0); s.uleb(T_new);
    s.str("env"); s.str("wasmjit_subtype");   s.u8(0); s.uleb(T_i32x2);
    s.str("env"); s.str("wasmjit_interface_callee"); s.u8(0); s.uleb(T_i32x2);
    s.str("env"); s.str("wasmjit_osr_enter"); s.u8(0); s.uleb(T_i32x1);
    s.str("env"); s.str("wasmjit_register_finalizer"); s.u8(0); s.uleb(T_safepoint);   // same type
    module.section(2, s);
  }
  // functions: the compiled method and _emscripten_tls_init (required from
  // every side module with pthreads; returns 0 = no TLS)
  { WasmBuf s; s.uleb(2); s.uleb(T_entry); s.uleb(T_void_i32); module.section(3, s); }
  { WasmBuf s; s.uleb(2);
    s.str("wasmjit_entry"); s.u8(0); s.uleb(H_count);
    s.str("_emscripten_tls_init"); s.u8(0); s.uleb(H_count + 1);
    module.section(7, s); }
  { WasmBuf s; s.uleb(2);
    s.uleb(body.length()); s.append(body);
    s.uleb(4); s.uleb(0); s.u8(OP_i32_const); s.u8(0); s.u8(OP_end);      // tls_init: () -> 0
    module.section(10, s); }
  return true;
}

// ---------------------------------------------------------------------------
// Installation

// dlopen makes every thread load the module into its own instance, which a
// thread only does while it waits; do it at a safepoint, when all Java
// threads are parked.
class VM_WasmJitLoad : public VM_Operation {
  const char* _path;
  void* _fn;
 public:
  VM_WasmJitLoad(const char* path) : _path(path), _fn(nullptr) {}
  VMOp_Type type() const { return VMOp_ForceSafepoint; }
  void doit() {
    void* handle = ::dlopen(_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      fprintf(stderr, "[wasmjit] dlopen(%s) failed: %s\n", _path, ::dlerror());
      return;
    }
    _fn = ::dlsym(handle, "wasmjit_entry");
  }
  void* fn() const { return _fn; }
};

void WasmJit::compile_and_install(Method* method, JavaThread* thread) {
  ResourceMark rm(thread);
  methodHandle mh(thread, method);
  jlong t0 = os::javaTimeNanos();
  WasmJitCompiler c(mh);
  WasmBuf module;
  if (!c.compile(module)) {
    if (_log) fprintf(stderr, "[wasmjit] failed %s: %s\n", method->name_and_sig_as_C_string(), c.failure());
    return;
  }
  char path[512];
  int n = AtomicAccess::add(&wasmjit_module_counter, 1);
  jio_snprintf(path, sizeof(path), "%s/wasmjit_%d_%d.so", wasmjit_dir, os::current_process_id(), n);
  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    if (_log) fprintf(stderr, "[wasmjit] cannot write %s\n", path);
    return;
  }
  fwrite(module.data(), 1, module.length(), f);
  fclose(f);
  jlong t1 = os::javaTimeNanos();
  VM_WasmJitLoad op(path);
  VMThread::execute(&op);
  ::unlink(path);
  jlong t2 = os::javaTimeNanos();
  if (op.fn() == nullptr) {
    return;
  }
  WasmJitEntry* entry = (WasmJitEntry*) os::malloc(sizeof(WasmJitEntry), mtCode);
  entry->entry_point = (address) op.fn();
  entry->interp_entry = method->interpreter_entry();
  entry->bailouts = 0;
  method->set_interpreter_entry((address) entry);
  method->set_is_not_c2_compilable();   // marker: has Wasm code (see wasm_entry_of)
  if (_log) {
    fprintf(stderr, "[wasmjit] %s: %d bytes bytecode -> %d bytes wasm, %d blocks, %d bailout sites, "
                    "compile %.2f ms, load %.2f ms\n",
            method->name_and_sig_as_C_string(), method->code_size(), module.length(), c.nblocks(),
            c.bail_sites(), (t1 - t0) / 1e6, (t2 - t1) / 1e6);
  }
}

static address wasm_entry_of(Method* method) {
  if (!method->is_not_c2_compilable()) return nullptr;
  return ((ZeroEntry*) method->from_interpreted_entry())->entry_point();
}

bool WasmJit::osr(JavaThread* thread, BytecodeInterpreter* istate) {
  Method* method = istate->method();
  address fn = wasm_entry_of(method);
  if (fn == nullptr) {
    if (method->is_not_c1_compilable() || method->is_synchronized() ||
        JvmtiExport::can_post_interpreter_events()) {
      return false;
    }
    method->set_is_not_c1_compilable();   // compile at most once
    // The frame was decached (bcp, tos) by the interpreter; make it walkable.
    thread->set_last_Java_frame();
    {
      ThreadInVMfromJava tiv(thread);
      compile_and_install(method, thread);
    }
    thread->reset_last_Java_frame();
    fn = wasm_entry_of(method);
    if (fn == nullptr) return false;
  }
  int bci = istate->bcp() - method->code_base();
  if (_log) {
    ResourceMark rm(thread);
    fprintf(stderr, "[wasmjit] OSR %s at bci %d\n", method->name_and_sig_as_C_string(), bci);
  }
  typedef int (*entry_t)(Method*, intptr_t, JavaThread*);
  ((entry_t) fn)(method, -(bci + 1), thread);
  return true;
}

void WasmJit::on_interpreted_invocation(Method* method, JavaThread* thread) {
  if (wasmjit_state == 0) initialize();
  if (!_enabled || method->is_not_c1_compilable()) return;
  MethodCounters* mcs = method->method_counters();
  if (mcs == nullptr) {
    // Called before the frame is built, with the caller's frame on top and the
    // frame anchor cleared: make the stack walkable for the VM transition.
    thread->set_last_Java_frame();
    {
      ThreadInVMfromJava tiv(thread);
      mcs = Method::build_method_counters(thread, method);
    }
    thread->reset_last_Java_frame();
    if (mcs == nullptr) return;
  }
  InvocationCounter* ic = mcs->invocation_counter();
  ic->increment();
  if (ic->count() != (uint) _threshold) return;

  if (method->is_native() || method->is_abstract() || method->is_synchronized() ||
      method->code_size() > 16000 || JvmtiExport::can_post_interpreter_events()) {
    method->set_is_not_c1_compilable();
    return;
  }
  method->set_is_not_c1_compilable();   // compile at most once
  thread->set_last_Java_frame();
  {
    ThreadInVMfromJava tiv(thread);
    compile_and_install(method, thread);
  }
  thread->reset_last_Java_frame();
}

#endif // __EMSCRIPTEN__
