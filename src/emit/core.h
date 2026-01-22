#ifndef EMIT_CORE_H
#define EMIT_CORE_H

#include "../emit.h"
#include "../sema.h" // for Type, TYPE_* enums, sema_arena, etc.
#include <stdio.h>
#include <string.h>

// forward‐declaration of the slice‐recording helper
const char *emit_slice_type_definition(Type *type);

void c_name_for_type(Type *t, char *out, size_t cap);

/*— where all output goes —*/
static FILE *output_file;
#define EMIT(...) fprintf(output_file, __VA_ARGS__)

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
      
      char sliceBuf[128];
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
  char sliceBuf[128];
  c_name_for_type(ty, sliceBuf, sizeof sliceBuf);

  // Emit initializer:
  // Use direct brace initialization to support array fields in struct
  EMIT("(%s){ .data = { ", sliceBuf);
  for (size_t i = 0; i < fixed_len; i++) {
      unsigned v = (i < bytes_len) ? (unsigned)bytes[i] : 0u;
      EMIT("0x%02X", v);
      if (i + 1 < fixed_len) EMIT(", ");
  }
  EMIT(" } }");
  return true;
}

// Helper to coerce Fixed Array/Slice variables to Sentinel/Dynamic Slices
// e.g. var x = "foo"; f(x); -> f( (Slice_u8_0){ .len=3, .data=x.data } )
// Forward declare emit_expr
struct Expr;
void emit_expr(struct Expr *expr, int depth);

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
        char targetBuf[128];
        c_name_for_type(target, targetBuf, sizeof targetBuf);
        
        EMIT("(%s){ .len = %zu, .data = ", targetBuf, src_len);
        emit_expr(source, depth);
        EMIT(".data }"); 
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
  static char buf[256];
  int len = id->length < sizeof(buf) - 1 ? id->length : sizeof(buf) - 1;
  memcpy(buf, id->name, len);
  buf[len] = '\0';

  Symbol *sym = sema_lookup(buf);
  return sym ? sym->c_name : buf;
}

static bool is_primitive_type(Type *t) {
    if (!t) return false;
    // Unwrap comptime wrapper only (mode is now just a field)
    while (t) {
        if (t->kind == TYPE_COMPTIME) { t = t->element_type; continue; }
        break;
    }
    
    if (t->kind == TYPE_POINTER) return true;
    if (t->kind == TYPE_SLICE) return true; // Slices are small {ptr, len}
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
        
        // Enums are primitives (integers)
        Symbol *sym = sema_lookup(c_name_for_id(base));
        if (sym && sym->decl && sym->decl->kind == DECL_ENUM) return true;
    }
    return false;
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

  // --- Unwrap transparent wrappers (only TYPE_COMPTIME is a wrapper now) ---
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

    // Recognize unsigned/signed fixed-width integer names:
    // u8,u16,u32,u64  -> uint8_t,uint16_t,uint32_t,uint64_t
    // i8,i16,i32,i64  -> int8_t,int16_t,int32_t,int64_t
    // isize, usize    -> intptr_t, uintptr_t (platform pointer-sized)
    char base_name[128];
    
    if (base->length == 2 && strncmp(base->name, "u8", 2) == 0) {
      snprintf(base_name, sizeof(base_name), "uint8_t");
    } else if (base->length == 3 && strncmp(base->name, "u16", 3) == 0) {
      snprintf(base_name, sizeof(base_name), "uint16_t");
    } else if (base->length == 3 && strncmp(base->name, "u32", 3) == 0) {
      snprintf(base_name, sizeof(base_name), "uint32_t");
    } else if (base->length == 3 && strncmp(base->name, "u64", 3) == 0) {
      snprintf(base_name, sizeof(base_name), "uint64_t");
    } else if (base->length == 2 && strncmp(base->name, "i8", 2) == 0) {
      snprintf(base_name, sizeof(base_name), "int8_t");
    } else if (base->length == 3 && strncmp(base->name, "i16", 3) == 0) {
      snprintf(base_name, sizeof(base_name), "int16_t");
    } else if (base->length == 3 && strncmp(base->name, "i32", 3) == 0) {
      snprintf(base_name, sizeof(base_name), "int32_t");
    } else if (base->length == 3 && strncmp(base->name, "i64", 3) == 0) {
      snprintf(base_name, sizeof(base_name), "int64_t");
    } else if (base->length == 5 && strncmp(base->name, "isize", 5) == 0) {
      snprintf(base_name, sizeof(base_name), "intptr_t");
    } else if (base->length == 5 && strncmp(base->name, "usize", 5) == 0) {
      snprintf(base_name, sizeof(base_name), "uintptr_t");
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

  case TYPE_POINTER: {
    char tgt[128];
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

  default:
    fprintf(stderr, "emit error: unhandled type kind %d\n", t->kind);
    exit(1);
  }
}




// Emit a C type (simple, array, slice, pointer, etc.)
static void emit_type(Type *type) {
  if (!type) return;
  char buf[128];
  c_name_for_type(type, buf, sizeof buf);
  EMIT("%s", buf);
}




#endif // EMIT_CORE_H