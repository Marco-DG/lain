#ifndef EMIT_CORE_H
#define EMIT_CORE_H

#include "../emit.h"
#include "../sema.h" // for Type, TYPE_* enums, sema_arena, etc.
#include <stdio.h>
#include <string.h>

// forward‐declaration of the slice‐recording helper
const char *emit_slice_type_definition(Type *type);
// forward decls: defined later in lain_header.h (which includes this file first)
static void record_vector_type(const char *vecName, const char *c_elem, int bytes);
static const char *canonical_base_name(Id *base_id, char *out, size_t cap);

void c_name_for_type(Type *t, char *out, size_t cap);
// Build a C function-pointer declarator into `out`: "R (*<name>)(P..)".
// `name` is "" for an abstract type (casts) or the variable/param C name.
static void c_name_for_fnptr(Type *t, const char *name, char *out, size_t cap);

/*— where all output goes —*/
static FILE *output_file;
static const char *emit_source_filename = NULL;
#define EMIT(...) fprintf(output_file, __VA_ARGS__)

/*— defer mechanics —*/
#define MAX_DEFERS 256
#define MAX_LOOPS 64
static Stmt *emit_defer_stack[MAX_DEFERS];
static int emit_defer_count = 0;

static int loop_defer_base[MAX_LOOPS];
static int loop_depth = 0;

/*— indentation —*/
static inline void emit_indent(int depth) {
  for (int i = 0; i < depth; i++)
    EMIT("    ");
}


// Emit a fixed-length byte initializer for a TYPE_ARRAY or a TYPE_SLICE
// that encodes a compile-time length in sentinel_len. Returns true if handled.
static bool emit_fixed_string_init(Type *ty, Expr *rhs, int depth) {
  (void)depth;
  if (!ty || !rhs || rhs->kind != EXPR_STRING) return false;

  size_t fixed_len = 0;
  bool is_fixed_like = false;

  if (ty->kind == TYPE_ARRAY && ty->array_len >= 0) {
      is_fixed_like = true;
      fixed_len = (size_t)ty->array_len;
  } else if (ty->kind == TYPE_SLICE && ty->sentinel_str == NULL && ty->sentinel_len > 0) {
      // Fixed-length slice encoded in sentinel_len
      is_fixed_like = true;
      fixed_len = (size_t)ty->sentinel_len;
  } else if (ty->kind == TYPE_SLICE && (ty->sentinel_str != NULL || ty->sentinel_is_string)) {
      // Sentinel-terminated slice (e.g. u8[:0]) initialized by string literal
      // Coerce fixed string into sentinel slice struct
      const unsigned char *bytes = (const unsigned char*)rhs->as.string_expr.value;
      size_t bytes_len = (size_t)rhs->as.string_expr.length;
      
      char sliceBuf[256];
      c_name_for_type(ty, sliceBuf, sizeof sliceBuf);
      
      // Emit compound literal: (Slice_u8_0){ .len = N, .data = (uint8_t[]){ ... } }
      // Note: we need to cast the array literal to pointer
      if (ty->mode == MODE_MUTABLE) {
           // Mutable slice? "var s u8[:0] = ...".
           // String literals are usually const data. C allows warning but valid.
      }

      EMIT("(%s){ .len = %zu, .data = (uint8_t[]){ ", sliceBuf, bytes_len);
      for (size_t i = 0; i < bytes_len; i++) {
          EMIT("0x%02X, ", bytes[i]);
      }
      // Emit the sentinel if it's numeric 0 (common case)
      // If sentinel is string, we should append it.
      // But string literal in code already has null terminator?
      // Wait, rhs->length includes valid chars.
      // We should append the sentinel value explicitly to be safe.
      // If Sentinel is 0:
      EMIT("0 } }"); 
      return true;
  }

  if (!is_fixed_like) return false;

  const unsigned char *bytes = (const unsigned char*)rhs->as.string_expr.value;
  size_t bytes_len = (size_t)rhs->as.string_expr.length;

  // typedef name for this fixed type, e.g. "Fixed_u8_5"
  char sliceBuf[256];
  c_name_for_type(ty, sliceBuf, sizeof sliceBuf);

  // Emit C array initializer: { 0x78, 0x20, ... }
  // (No struct wrapper — u8[N] is now a native C array)
  (void)sliceBuf;
  EMIT("{ ");
  for (size_t i = 0; i < fixed_len; i++) {
      unsigned v = (i < bytes_len) ? (unsigned)bytes[i] : 0u;
      EMIT("0x%02X", v);
      if (i + 1 < fixed_len) EMIT(", ");
  }
  EMIT(" }");
  return true;
}

// Helper to coerce Fixed Array/Slice variables to Sentinel/Dynamic Slices
// e.g. var x = "foo"; f(x); -> f( (Slice_u8_0){ .len=3, .data=x.data } )
// Forward declare emit_expr
struct Expr;
void emit_expr(struct Expr *expr, int depth);
// Forward declare emit_stmt so EXPR_TRY (emit/expr.h) can flush pending defers on
// its propagate-`return` path — the same cleanup a STMT_RETURN runs.
struct Stmt;
void emit_stmt(struct Stmt *stmt, int depth);

static bool emit_slice_coercion(Type *target, Expr *source, int depth) {
    if (!target || !source) return false;
    
    // Forward to string init if it is a string literal
    if (source->kind == EXPR_STRING) {
        return emit_fixed_string_init(target, source, depth);
    }

    // Only coerce if Target is Sentinel Slice or Dynamic Slice
    bool target_is_sentinel = (target->kind == TYPE_SLICE && (target->sentinel_str || target->sentinel_is_string));
    bool target_is_dynamic = (target->kind == TYPE_ARRAY && target->array_len == -1) ||
                             (target->kind == TYPE_SLICE && !target_is_sentinel && !target->sentinel_len); // Basic slice

    if (!target_is_sentinel && !target_is_dynamic) return false;

    // Check Source Type (Must be Fixed Array/Slice)
    Type *st = source->type;
    // Fallback: if type not set on expr, get it from the declaration
    if (!st && source->decl && source->decl->kind == DECL_VARIABLE) {
        st = source->decl->as.variable_decl.type;
    }
    size_t src_len = 0;
    bool src_is_fixed = false;
    
    // Unwrap if needed?
    if (st && st->kind == TYPE_ARRAY && st->array_len >= 0) {
        src_is_fixed = true;
        src_len = (size_t)st->array_len;
    } else if (st && st->kind == TYPE_SLICE && st->sentinel_str == NULL && st->sentinel_len > 0) {
        src_is_fixed = true;
        src_len = (size_t)st->sentinel_len;
    }

    if (src_is_fixed) {
        // Emit Coercion
        char targetBuf[256];
        c_name_for_type(target, targetBuf, sizeof targetBuf);

        EMIT("(%s){ .len = %zu, .data = ", targetBuf, src_len);
        emit_expr(source, depth);
        // A native C array (TYPE_ARRAY, e.g. a local `u8[N]`) decays to a pointer
        // directly. A fixed TYPE_SLICE (a Fixed_<T>_N struct — e.g. a string
        // literal binding `var s = "..."`) is a struct with a `.data[N]` member,
        // so it needs `.data` to yield the pointer.
        if (st && st->kind == TYPE_SLICE) EMIT(".data");
        EMIT(" }");
        return true;
    }
    return false;
}



// ────────────────────────────────────────────────────────────────────────────
// Helper: get the root base_type for complex/wrapped types
Id *get_root_base_type(Type *type) {
  while (type) {
    if (type->kind == TYPE_ARRAY || type->kind == TYPE_SLICE) {
      type = type->element_type;
      continue;
    }
    if (type->kind == TYPE_COMPTIME) {
      // comptime is a transparent wrapper: unwrap the inner element_type
      type = type->element_type;
      continue;
    }
    // With new OwnershipMode system, mode is just a field, not a wrapper type
    // So we don't need to unwrap TYPE_MOVE/TYPE_MUT anymore
    break;
  }
  return type ? type->base_type : NULL;
}



const char *c_name_for_id(Id *id) {
  static char buf[1024];
  int len = id->length < (int)sizeof(buf) - 2 ? id->length : (int)sizeof(buf) - 2;
  int offset = 0;

  // C identifiers cannot start with a digit: prefix with '_'
  if (len > 0 && id->name[0] >= '0' && id->name[0] <= '9') {
      buf[0] = '_';
      offset = 1;
  }

  memcpy(buf + offset, id->name, len);
  buf[len + offset] = '\0';

  Symbol *sym = sema_lookup(buf);
  if (sym) {
      return sym->c_name;
  }
  
  for (int i = offset; i < len + offset; i++) {
      char c = buf[i];
      if (c == '.') buf[i] = '_';
      // Sanitize any non-identifier char (spaces, hyphens, etc.) to underscore
      else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_')) {
          buf[i] = '_';
      }
  }
  return buf;
}


static bool is_primitive_type(Type *t) {
    if (!t) return false;
    // Unwrap comptime wrappers.
    while (t) {
        if (t->kind == TYPE_COMPTIME) { t = t->element_type; continue; }
        break;
    }
    
    if (t->kind == TYPE_POINTER) return true;
    if (t->kind == TYPE_SLICE) return true; // Slices are small {ptr, len}
    if (t->kind == TYPE_VECTOR) return true; // SIMD vectors live in registers — by value
    if (t->kind == TYPE_SIMPLE) {
        Id *base = t->base_type;
        if (!base) return false;
        // Check for known primitives
        if (base->length == 3 && strncmp(base->name, "int", 3) == 0) return true;
        if (base->length == 2 && strncmp(base->name, "u8", 2) == 0) return true;
        if (base->length == 3 && strncmp(base->name, "u16", 3) == 0) return true;
        if (base->length == 3 && strncmp(base->name, "u32", 3) == 0) return true;
        if (base->length == 3 && strncmp(base->name, "u64", 3) == 0) return true;
        if (base->length == 2 && strncmp(base->name, "i8", 2) == 0) return true;
        if (base->length == 3 && strncmp(base->name, "i16", 3) == 0) return true;
        if (base->length == 3 && strncmp(base->name, "i32", 3) == 0) return true;
        if (base->length == 3 && strncmp(base->name, "i64", 3) == 0) return true;
        if (base->length == 5 && strncmp(base->name, "isize", 5) == 0) return true;
        if (base->length == 5 && strncmp(base->name, "usize", 5) == 0) return true;
        if (base->length == 4 && strncmp(base->name, "bool", 4) == 0) return true;
        if (base->length == 4 && strncmp(base->name, "char", 4) == 0) return true;
        if (base->length == 5 && strncmp(base->name, "float", 5) == 0) return true;
        if (base->length == 3 && strncmp(base->name, "f32", 3) == 0) return true;
        if (base->length == 3 && strncmp(base->name, "f64", 3) == 0) return true;
        // Arbitrary bit-width integers: u<N> / i<N> (u1, u2, u7, i3, ...). These
        // are scalar primitives passed by value — without this they were treated
        // as aggregates and passed by pointer, so a call emitted `&(literal)`.
        if (base->length >= 2 && (base->name[0] == 'u' || base->name[0] == 'i')) {
            bool all_digits = true;
            for (isize k = 1; k < base->length; k++)
                if (base->name[k] < '0' || base->name[k] > '9') { all_digits = false; break; }
            if (all_digits) return true;
        }

        // Enums are primitives (integers)
        Symbol *sym = sema_lookup(c_name_for_id(base));
        if (sym && sym->decl && sym->decl->kind == DECL_ENUM) return true;
        // Scalar-typedef'd primitive aliases (emit-side registry).
        if (is_scalar_typedef(c_name_for_id(base))) return true;
    }
    return false;
}

// True iff a TYPE_ARRAY with a non-primitive element type should be emitted
// as a native C array (ElemType name[N]) rather than a Fixed_T_N struct.
// User-defined element types cannot be used in Fixed_ structs defined in
// lain.h because the struct body is not yet complete at include time.
static bool is_user_type_fixed_array(Type *ty) {
    return ty && ty->kind == TYPE_ARRAY && ty->array_len > 0 &&
           ty->element_type && !is_primitive_type(ty->element_type);
}

// Fase 7: true iff d is a dynamic-array (i32[]) function parameter.
// These are decomposed to (size_t __len_PARAM,)? T * [restrict] PARAM.
static bool is_dynarray_param_decl(Decl *d) {
    if (!d || d->kind != DECL_VARIABLE) return false;
    if (!d->as.variable_decl.is_parameter) return false;
    Type *t = d->as.variable_decl.type;
    return t && t->kind == TYPE_ARRAY && t->array_len == -1;
}

// Case-arm payload bindings currently in scope during codegen. A binding
// (`case s { Some(v): … v … }`) is a NULL-typed local that isn't tracked in the
// symbol table, so the undeclared-identifier check (emit/expr.h EXPR_IDENTIFIER)
// would otherwise mistake it for a typo. The match emit pushes each arm's
// binding names here for the duration of that arm's body.
static const char *emit_binding_name[256];
static int         emit_binding_len[256];
static int         emit_binding_depth = 0;
static bool emit_name_is_binding(const char *name, int len) {
    for (int i = 0; i < emit_binding_depth; i++)
        if (emit_binding_len[i] == len &&
            strncmp(emit_binding_name[i], name, len) == 0) return true;
    return false;
}
static void emit_push_binding(const char *name, int len) {
    if (emit_binding_depth < 256) {
        emit_binding_name[emit_binding_depth] = name;
        emit_binding_len[emit_binding_depth]  = len;
        emit_binding_depth++;
    }
}

// Suppress the undeclared-identifier check while emitting expressions that live
// inside a TYPE rather than a value position (e.g. a VLA's size expression
// `u8[n]`): those identifiers reference real declarations but are not resolved
// as value nodes, so they carry no type and would otherwise be mis-flagged.
static bool emit_suppress_undeclared = false;

// A local `var a T[n]` VLA: an OWNED native stack array (a real `T a[n]`), not
// a sized-slice view. Like a native fixed array it decays to a bare pointer and
// indexes natively; its `.len` is the size expression `n`.
static bool is_vla_local_decl(Decl *d) {
    if (!d || d->kind != DECL_VARIABLE) return false;
    if (d->as.variable_decl.is_parameter) return false;
    Type *t = d->as.variable_decl.type;
    return t && t->is_vla;
}

// A dynamic-array param carries its length in a runtime `__len_PARAM` argument
// UNLESS it has a CONCRETE size binding (size_relop '==', e.g. i32[n],
// i32[out.len], i32[m+n] — there the length IS that expression). A pure size
// CONSTRAINT (i32[> 0], i32[>= 2]) only BOUNDS the length; the actual length is
// unknown at compile time, so it must be threaded at runtime exactly like a
// plain slice i32[]. Emitting the bound as `.len` was a miscompile: `buf.len`
// on an `*i32[> 0]` param rendered as the literal `0`, so `x % buf.len` divided
// by zero at runtime on a program VRA had "proven" safe.
static bool dynarray_param_has_runtime_len(Type *t) {
    if (!t || t->kind != TYPE_ARRAY || t->array_len != -1) return false;
    return t->size_expr == NULL || t->size_relop != TOKEN_EQUAL_EQUAL;
}

/*───────────────────────────────────────────────────────────────╗
│ Helper: emit the C-decl name for *any* semantic Type*        │
╚───────────────────────────────────────────────────────────────*/
// Emit the C-decl name for *any* semantic Type*
void c_name_for_type(Type *t, char *out, size_t cap) {
  if (!t) {
    snprintf(out, cap, "/*<unknown-type>*/");
    return;
  }

  // --- Unwrap transparent wrappers (TYPE_COMPTIME). ---
  Type *u = t;
  while (u) {
    if (u->kind == TYPE_COMPTIME) {
      if (u->element_type) { u = u->element_type; continue; }
      break;
    }
    break;
  }
  t = u;

  // Handle OwnershipMode for C code generation:
  // MODE_SHARED (shared ref) -> const T* for structs, T for primitives
  // MODE_MUTABLE (mut ref)   -> T* for all types
  // MODE_OWNED (move)        -> T (value type)
  
  bool is_mutable_ref = (t->mode == MODE_MUTABLE);
  bool is_shared_ref = (t->mode == MODE_SHARED);
  bool is_owned = (t->mode == MODE_OWNED);
  (void)is_shared_ref; // used in future for const T*
  (void)is_owned;

  switch (t->kind) {
  case TYPE_SIMPLE: {
    Id *base = t->base_type;
    if (!base) {
      snprintf(out, cap, "/*<anon-simple>*/");
      return;
    }

    // Recognize generic iN / uN names (N = 1..64) → smallest C type
    // containing N bits. Q-002 Paradigm B:
    //   N ∈ [1,8]   → (u)int8_t
    //   N ∈ [9,16]  → (u)int16_t
    //   N ∈ [17,32] → (u)int32_t
    //   N ∈ [33,64] → (u)int64_t
    // The legacy names u8/u16/u32/u64/i8/i16/i32/i64 are a special case
    // of this rule (their N is power-of-two).
    char base_name[256];

    // Try to parse iN / uN: 'i' or 'u' followed by 1-2 digits.
    int parsed_bits = 0;
    char parsed_sign = 0;
    if (base->length >= 2 && base->length <= 3
        && (base->name[0] == 'i' || base->name[0] == 'u')) {
      // Special-case: isize/usize handled below.
      if (!(base->length == 5)) {
        bool all_digits = true;
        for (isize k = 1; k < base->length; k++) {
          if (base->name[k] < '0' || base->name[k] > '9') { all_digits = false; break; }
        }
        if (all_digits) {
          int v = 0;
          for (isize k = 1; k < base->length; k++) v = v * 10 + (base->name[k] - '0');
          if (v >= 1 && v <= 64) {
            parsed_bits = v;
            parsed_sign = base->name[0];
          }
        }
      }
    }

    if (parsed_bits > 0) {
      int container =
          (parsed_bits <= 8)  ?  8 :
          (parsed_bits <= 16) ? 16 :
          (parsed_bits <= 32) ? 32 :
                                64;
      snprintf(base_name, sizeof(base_name),
               "%sint%d_t", parsed_sign == 'u' ? "u" : "", container);
    } else if (base->length == 5 && strncmp(base->name, "isize", 5) == 0) {
      // Spec §2.2.1: isize ↔ ptrdiff_t, usize ↔ size_t (the idiomatic size/index
      // types). Same width as {u,}intptr_t but the correct C types — and size_t
      // matches libc size/malloc signatures, avoiding spurious interop conflicts.
      snprintf(base_name, sizeof(base_name), "ptrdiff_t");
    } else if (base->length == 5 && strncmp(base->name, "usize", 5) == 0) {
      snprintf(base_name, sizeof(base_name), "size_t");
    } else if (base->length == 3 && strncmp(base->name, "int", 3) == 0) {
      // `int` is an alias of i32, documented for ergonomic use.
      snprintf(base_name, sizeof(base_name), "int32_t");
    } else if (base->length == 4 && strncmp(base->name, "bool", 4) == 0) {
      snprintf(base_name, sizeof(base_name), "_Bool");
    } else if (base->length == 3 && strncmp(base->name, "f32", 3) == 0) {
      snprintf(base_name, sizeof(base_name), "float");
    } else if (base->length == 3 && strncmp(base->name, "f64", 3) == 0) {
      snprintf(base_name, sizeof(base_name), "double");
    } else if (base->length == 5 && strncmp(base->name, "float", 5) == 0) {
      // `float` is an alias of f32 (C float), documented for ergonomics.
      snprintf(base_name, sizeof(base_name), "float");
    } else {
      // fallback to symbol lookup (enums, structs, locals…)
      const char *cname = c_name_for_id(base);
      snprintf(base_name, sizeof(base_name), "%s", cname);
    }
    
    // Add pointer for mutable references
    if (is_mutable_ref) {
      snprintf(out, cap, "%s *", base_name);
    } else {
      snprintf(out, cap, "%s", base_name);
    }
    return;
  }

  case TYPE_ARRAY:
  case TYPE_SLICE: {
    const char *sliceName = emit_slice_type_definition(t);
    if (is_mutable_ref) {
      snprintf(out, cap, "%s *", sliceName);
    } else {
      snprintf(out, cap, "%s", sliceName);
    }
    return;
  }

  case TYPE_VECTOR: {
    // Vec(N, T) → a GCC/Clang `vector_size` typedef `Vec_<N>_<elem>`.
    Type *elem = t->element_type;
    char c_elem[128];
    c_name_for_type(elem, c_elem, sizeof c_elem);            // e.g. "int32_t"
    char rawname[64];
    canonical_base_name(elem ? elem->base_type : NULL, rawname, sizeof rawname); // "i32"
    // Element byte size from its C primitive name.
    int esz = 4;
    if      (!strcmp(c_elem,"uint8_t")  || !strcmp(c_elem,"int8_t")  || !strcmp(c_elem,"_Bool")) esz = 1;
    else if (!strcmp(c_elem,"uint16_t") || !strcmp(c_elem,"int16_t")) esz = 2;
    else if (!strcmp(c_elem,"uint32_t") || !strcmp(c_elem,"int32_t") || !strcmp(c_elem,"float"))  esz = 4;
    else if (!strcmp(c_elem,"uint64_t") || !strcmp(c_elem,"int64_t") || !strcmp(c_elem,"double")) esz = 8;
    int lanes = (int)t->array_len;
    char vecName[128];
    snprintf(vecName, sizeof vecName, "Vec_%d_%s", lanes, rawname);
    record_vector_type(vecName, c_elem, lanes * esz);
    if (is_mutable_ref) snprintf(out, cap, "%s *", vecName);
    else                snprintf(out, cap, "%s", vecName);
    return;
  }

  case TYPE_POINTER: {
    // *T[N]: thin pointer — emit const T *, not const Fixed_T_N *
    // N is tracked in the type system for bounds checking only; no runtime overhead.
    if (t->element_type && t->element_type->kind == TYPE_ARRAY &&
        t->element_type->array_len >= 0) {
        char elem[256];
        c_name_for_type(t->element_type->element_type, elem, sizeof elem);
        if (t->mode == MODE_MUTABLE || t->mode == MODE_OWNED)
            snprintf(out, cap, "%s *", elem);
        else
            snprintf(out, cap, "const %s *", elem);
        return;
    }

    char tgt[256];
    c_name_for_type(t->element_type, tgt, sizeof tgt);

    // Check mode for const correctness
    if (t->mode == MODE_MUTABLE || t->mode == MODE_OWNED) {
        snprintf(out, cap, "%s *", tgt);
    } else {
        // Shared pointer -> const T *
        snprintf(out, cap, "const %s *", tgt);
    }
    return;
  }

  case TYPE_COMPTIME: {
    // Should have been unwrapped above, but handle gracefully
    if (t->element_type) {
      c_name_for_type(t->element_type, out, cap);
      return;
    }
    snprintf(out, cap, "/*<unknown-comptime-type>*/");
    return;
  }

  case TYPE_FUNC: {
    // Abstract function-pointer type `R (*)(P..)` — valid in casts/sizeof.
    // Declarations that need the name inside use c_name_for_fnptr(t, name, …).
    c_name_for_fnptr(t, "", out, cap);
    return;
  }

  default:
    fprintf(stderr, "emit error: unhandled type kind %d\n", t->kind);
    exit(1);
  }
}

static void c_name_for_fnptr(Type *t, const char *name, char *out, size_t cap) {
  char rbuf[192];
  if (t->element_type) c_name_for_type(t->element_type, rbuf, sizeof rbuf);
  else                 snprintf(rbuf, sizeof rbuf, "void");

  char pbuf[512];
  if (!t->func_params) {
    snprintf(pbuf, sizeof pbuf, "void");
  } else {
    size_t off = 0; int first = 1;
    pbuf[0] = '\0';
    for (TypeList *p = t->func_params; p && off < sizeof pbuf; p = p->next) {
      char pb[192];
      c_name_for_type(p->type, pb, sizeof pb);
      int n = snprintf(pbuf + off, sizeof pbuf - off, "%s%s", first ? "" : ", ", pb);
      if (n > 0) off += (size_t)n;
      first = 0;
    }
  }
  snprintf(out, cap, "%s (*%s)(%s)", rbuf, name ? name : "", pbuf);
}




// Emit a C type (simple, array, slice, pointer, etc.)
static void emit_type(Type *type) {
  if (!type) return;
  char buf[256];
  c_name_for_type(type, buf, sizeof buf);
  EMIT("%s", buf);
}




#endif // EMIT_CORE_H