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
      // Some of your code-paths encode compile-time fixed length in TYPE_SLICE.sentinel_len
      is_fixed_like = true;
      fixed_len = (size_t)ty->sentinel_len;
  }

  if (!is_fixed_like) return false;

  const unsigned char *bytes = (const unsigned char*)rhs->as.string_expr.value;
  size_t bytes_len = (size_t)rhs->as.string_expr.length;

  // typedef name for this fixed type, e.g. "Fixed_u8_5"
  char sliceBuf[128];
  c_name_for_type(ty, sliceBuf, sizeof sliceBuf);

  // Emit compound literal: (Fixed_T_N){ .data = (uint8_t[]){ ... } }
  EMIT("(%s){ .data = (uint8_t[]){ ", sliceBuf);
  for (size_t i = 0; i < fixed_len; i++) {
      unsigned v = (i < bytes_len) ? (unsigned)bytes[i] : 0u;
      EMIT("0x%02X", v);
      if (i + 1 < fixed_len) EMIT(", ");
  }
  EMIT(" } }");
  return true;
}



// ────────────────────────────────────────────────────────────────────────────
// Helper: get the root base_type for complex/wrapped types
Id *get_root_base_type(Type *type) {
  while (type) {
    if (type->kind == TYPE_ARRAY || type->kind == TYPE_SLICE) {
      type = type->element_type;
      continue;
    }
    if (type->kind == TYPE_MOVE) {
      // unwrap move wrapper and keep looking
      // prefer move.base if present, otherwise fallback to element_type
      if (type->move.base) {
        type = type->move.base;
      } else {
        type = type->element_type;
      }
      continue;
    }
    if (type->kind == TYPE_COMPTIME) {
      // comptime is a transparent wrapper: unwrap the inner element_type
      type = type->element_type;
      continue;
    }
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

/*───────────────────────────────────────────────────────────────╗
│ Helper: emit the C‐decl name for *any* semantic Type*        │
╚───────────────────────────────────────────────────────────────*/
// Emit the C-decl name for *any* semantic Type*
void c_name_for_type(Type *t, char *out, size_t cap) {
  if (!t) {
    snprintf(out, cap, "/*<unknown-type>*/");
    return;
  }

  // --- Robust unwrapping of transparent wrappers ---
  Type *u = t;
  while (u) {
    if (u->kind == TYPE_MOVE) {
      if (u->move.base) { u = u->move.base; continue; }
      if (u->element_type) { u = u->element_type; continue; }
      break;
    }
    if (u->kind == TYPE_COMPTIME) {
      if (u->element_type) { u = u->element_type; continue; }
      if (u->move.base) { u = u->move.base; continue; }
      break;
    }
    break;
  }
  t = u;

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
    // Compare using exact lengths to avoid accidental prefix matches.
    if (base->length == 2 && strncmp(base->name, "u8", 2) == 0) {
      snprintf(out, cap, "uint8_t");
      return;
    }
    if (base->length == 3 && strncmp(base->name, "u16", 3) == 0) {
      snprintf(out, cap, "uint16_t");
      return;
    }
    if (base->length == 3 && strncmp(base->name, "u32", 3) == 0) {
      snprintf(out, cap, "uint32_t");
      return;
    }
    if (base->length == 3 && strncmp(base->name, "u64", 3) == 0) {
      snprintf(out, cap, "uint64_t");
      return;
    }
    if (base->length == 2 && strncmp(base->name, "i8", 2) == 0) {
      snprintf(out, cap, "int8_t");
      return;
    }
    if (base->length == 3 && strncmp(base->name, "i16", 3) == 0) {
      snprintf(out, cap, "int16_t");
      return;
    }
    if (base->length == 3 && strncmp(base->name, "i32", 3) == 0) {
      snprintf(out, cap, "int32_t");
      return;
    }
    if (base->length == 3 && strncmp(base->name, "i64", 3) == 0) {
      snprintf(out, cap, "int64_t");
      return;
    }
    if (base->length == 5 && strncmp(base->name, "isize", 5) == 0) {
      snprintf(out, cap, "intptr_t");
      return;
    }
    if (base->length == 5 && strncmp(base->name, "usize", 5) == 0) {
      snprintf(out, cap, "uintptr_t");
      return;
    }

    // fallback to symbol lookup (enums, structs, locals…)
    const char *cname = c_name_for_id(base);
    snprintf(out, cap, "%s", cname);
    return;
  }

  case TYPE_ARRAY:
  case TYPE_SLICE: {
    const char *sliceName = emit_slice_type_definition(t);
    snprintf(out, cap, "%s", sliceName);
    return;
  }

  case TYPE_POINTER: {
    char tgt[128];
    c_name_for_type(t->element_type, tgt, sizeof tgt);
    snprintf(out, cap, "%s *", tgt);
    return;
  }

  // Defensive: wrappers already unwrapped; but handle gracefully if we see them
  case TYPE_MOVE:
  case TYPE_COMPTIME: {
    if (t->move.base) {
      c_name_for_type(t->move.base, out, cap);
      return;
    }
    if (t->element_type) {
      c_name_for_type(t->element_type, out, cap);
      return;
    }
    snprintf(out, cap, "/*<unknown-wrapper-type>*/");
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