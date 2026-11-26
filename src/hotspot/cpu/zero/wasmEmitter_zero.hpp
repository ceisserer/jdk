/*
 * Minimal WebAssembly binary encoder for the experimental wasm32 baseline
 * compiler (wasmJit_zero.cpp). Produces Emscripten side modules: a dylink.0
 * section, imports of env.memory / env.__indirect_function_table and of
 * runtime helper functions by name, exported functions.
 */
#ifndef CPU_ZERO_WASMEMITTER_ZERO_HPP
#define CPU_ZERO_WASMEMITTER_ZERO_HPP

#ifdef __EMSCRIPTEN__

#include "utilities/growableArray.hpp"

#include <string.h>

class WasmBuf {
 public:
  GrowableArrayCHeap<u1, mtCode> _b;

  int length() const { return _b.length(); }
  const u1* data() const { return _b.adr_at(0); }

  void u8(int v) { _b.append((u1) v); }
  void uleb(uint64_t v) {
    do {
      u1 byte = v & 0x7f;
      v >>= 7;
      if (v != 0) byte |= 0x80;
      _b.append(byte);
    } while (v != 0);
  }
  void sleb(int64_t v) {
    bool more = true;
    while (more) {
      u1 byte = v & 0x7f;
      v >>= 7;  // arithmetic shift
      if ((v == 0 && (byte & 0x40) == 0) || (v == -1 && (byte & 0x40) != 0)) {
        more = false;
      } else {
        byte |= 0x80;
      }
      _b.append(byte);
    }
  }
  void f32(float f)  { u1 b[4]; memcpy(b, &f, 4); for (int i = 0; i < 4; i++) u8(b[i]); }
  void f64(double d) { u1 b[8]; memcpy(b, &d, 8); for (int i = 0; i < 8; i++) u8(b[i]); }
  void str(const char* s) { size_t n = strlen(s); uleb(n); for (size_t i = 0; i < n; i++) u8(s[i]); }
  void append(const WasmBuf& o) { for (int i = 0; i < o.length(); i++) _b.append(o._b.at(i)); }
  // Section with id and size prefix.
  void section(int id, const WasmBuf& body) { u8(id); uleb(body.length()); append(body); }
};

// Value types
enum WasmType { W_I32 = 0x7f, W_I64 = 0x7e, W_F32 = 0x7d, W_F64 = 0x7c };

// Opcodes used by the compiler
enum WasmOp {
  OP_unreachable = 0x00, OP_nop = 0x01, OP_block = 0x02, OP_loop = 0x03, OP_if = 0x04, OP_else = 0x05,
  OP_end = 0x0b, OP_br = 0x0c, OP_br_if = 0x0d, OP_br_table = 0x0e, OP_return = 0x0f,
  OP_call = 0x10, OP_call_indirect = 0x11, OP_drop = 0x1a, OP_select = 0x1b,
  OP_local_get = 0x20, OP_local_set = 0x21, OP_local_tee = 0x22,
  OP_i32_load = 0x28, OP_i64_load = 0x29, OP_f32_load = 0x2a, OP_f64_load = 0x2b,
  OP_i32_load8_s = 0x2c, OP_i32_load8_u = 0x2d, OP_i32_load16_s = 0x2e, OP_i32_load16_u = 0x2f,
  OP_i32_store = 0x36, OP_i64_store = 0x37, OP_f32_store = 0x38, OP_f64_store = 0x39,
  OP_i32_store8 = 0x3a, OP_i32_store16 = 0x3b,
  OP_i32_const = 0x41, OP_i64_const = 0x42, OP_f32_const = 0x43, OP_f64_const = 0x44,
  OP_i32_eqz = 0x45, OP_i32_eq = 0x46, OP_i32_ne = 0x47, OP_i32_lt_s = 0x48, OP_i32_lt_u = 0x49,
  OP_i32_gt_s = 0x4a, OP_i32_gt_u = 0x4b, OP_i32_le_s = 0x4c, OP_i32_le_u = 0x4d,
  OP_i32_ge_s = 0x4e, OP_i32_ge_u = 0x4f,
  OP_i64_eqz = 0x50, OP_i64_eq = 0x51, OP_i64_ne = 0x52, OP_i64_lt_s = 0x53, OP_i64_gt_s = 0x55,
  OP_f32_eq = 0x5b, OP_f32_ne = 0x5c, OP_f32_lt = 0x5d, OP_f32_gt = 0x5e,
  OP_f64_eq = 0x61, OP_f64_ne = 0x62, OP_f64_lt = 0x63, OP_f64_gt = 0x64,
  OP_i32_add = 0x6a, OP_i32_sub = 0x6b, OP_i32_mul = 0x6c, OP_i32_div_s = 0x6d, OP_i32_rem_s = 0x6f,
  OP_i32_and = 0x71, OP_i32_or = 0x72, OP_i32_xor = 0x73, OP_i32_shl = 0x74, OP_i32_shr_s = 0x75,
  OP_i32_shr_u = 0x76,
  OP_i64_add = 0x7c, OP_i64_sub = 0x7d, OP_i64_mul = 0x7e, OP_i64_div_s = 0x7f, OP_i64_rem_s = 0x81,
  OP_i64_and = 0x83, OP_i64_or = 0x84, OP_i64_xor = 0x85, OP_i64_shl = 0x86, OP_i64_shr_s = 0x87,
  OP_i64_shr_u = 0x88,
  OP_f32_neg = 0x8c, OP_f32_add = 0x92, OP_f32_sub = 0x93, OP_f32_mul = 0x94, OP_f32_div = 0x95,
  OP_f64_neg = 0x9a, OP_f64_add = 0xa0, OP_f64_sub = 0xa1, OP_f64_mul = 0xa2, OP_f64_div = 0xa3,
  OP_i32_wrap_i64 = 0xa7, OP_i64_extend_i32_s = 0xac, OP_i64_extend_i32_u = 0xad,
  OP_f32_convert_i32_s = 0xb2, OP_f32_convert_i64_s = 0xb4, OP_f32_demote_f64 = 0xb6,
  OP_f64_convert_i32_s = 0xb7, OP_f64_convert_i64_s = 0xb9, OP_f64_promote_f32 = 0xbb,
  OP_i32_extend8_s = 0xc0, OP_i32_extend16_s = 0xc1,
  OP_prefix_fc = 0xfc, OP_prefix_fe = 0xfe
};
// 0xfc sub-opcodes (saturating truncation: Java semantics for f2i/f2l/d2i/d2l)
enum { FC_i32_trunc_sat_f32_s = 0, FC_i32_trunc_sat_f64_s = 2, FC_i64_trunc_sat_f32_s = 4,
       FC_i64_trunc_sat_f64_s = 6 };
// 0xfe sub-opcodes (atomics, for volatile fields)
enum { FE_i32_atomic_load = 0x10, FE_i64_atomic_load = 0x11, FE_i32_atomic_store = 0x17,
       FE_i64_atomic_store = 0x18 };

#endif // __EMSCRIPTEN__
#endif // CPU_ZERO_WASMEMITTER_ZERO_HPP
