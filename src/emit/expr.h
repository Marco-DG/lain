#ifndef EMIT_EXPR_H
#define EMIT_EXPR_H

#include "../emit.h"
#include "ctor.h"
#include <stdbool.h>
#include <string.h> // for memcpy

// very naive: you must set `emitted_decls` to point at your
// DeclList before you call emit()
static DeclList *emitted_decls = NULL;

static DeclFunction *lookup_function_decl(Expr *callee);

// Find the DeclFunction AST node for a bare‐identifier callee.
// You need your global list of DeclList* (here called `emitted_decls`).
static DeclFunction *lookup_function_decl(Expr *callee) {
  if (callee->kind != EXPR_IDENTIFIER)
    return NULL;
  
  // Copy cname to avoid static buffer overwrite in c_name_for_id
  char search_name[256];
  const char *raw_cname = c_name_for_id(callee->as.identifier_expr.id);
  strncpy(search_name, raw_cname, sizeof(search_name));
  search_name[sizeof(search_name)-1] = '\0';

  for (DeclList *dl = emitted_decls; dl; dl = dl->next) {
    Decl *d = dl->decl;
    if (d->kind == DECL_FUNCTION) {
        const char *dname = c_name_for_id(d->as.function_decl.name);
        // fprintf(stderr, "[DEBUG]   checking against '%s'\n", dname);
        if (strcmp(dname, search_name) == 0) {
            return &d->as.function_decl;
        }
    }
  }
  return NULL;
}

// Emit a size_expr from a type annotation without calling sema_infer_expr.
// Size_expr nodes are parsed in a type context; their EXPR_IDENTs are never
// resolved by the body sema pass, so sema_infer_expr would crash on them.
// We emit by reading raw AST names: x.len → __len_x, literals → value, etc.
static void emit_size_expr(Expr *se, int depth) {
    (void)depth;
    if (!se) return;
    if (se->kind == EXPR_MEMBER &&
        se->as.member_expr.member->length == 3 &&
        strncmp(se->as.member_expr.member->name, "len", 3) == 0 &&
        se->as.member_expr.target->kind == EXPR_IDENTIFIER) {
        Id *ref = se->as.member_expr.target->as.identifier_expr.id;
        EMIT("__len_%.*s", (int)ref->length, ref->name);
        return;
    }
    if (se->kind == EXPR_BINARY) {
        TokenKind op = se->as.binary_expr.op;
        emit_size_expr(se->as.binary_expr.left, depth);
        if (op == TOKEN_PLUS)        EMIT(" + ");
        else if (op == TOKEN_MINUS)  EMIT(" - ");
        else if (op == TOKEN_ASTERISK) EMIT(" * ");
        else if (op == TOKEN_SLASH)    EMIT(" / ");
        else                         EMIT(" ? ");
        emit_size_expr(se->as.binary_expr.right, depth);
        return;
    }
    if (se->kind == EXPR_LITERAL) {
        EMIT("%ld", (long)se->as.literal_expr.value);
        return;
    }
    if (se->kind == EXPR_IDENTIFIER) {
        Id *id = se->as.identifier_expr.id;
        EMIT("%.*s", (int)id->length, id->name);
        return;
    }
}

// Emit an expression at given indent‐depth
void emit_expr(Expr *expr, int depth);

void emit_expr(Expr *expr, int depth) {
  (void)depth;
  if (!expr)
    return;

  // ─────────────────────────────────────────────────────────────
  // Make sure the type‐checker/semantic pass has run on this node
  // before we ever try to access expr->type. Without this, indexing
  // into something like `text[...]` will still have expr->type == NULL.
  sema_resolve_expr(expr);
  sema_infer_expr(expr);
  // ─────────────────────────────────────────────────────────────

  switch (expr->kind) {
  case EXPR_LITERAL:
    EMIT("%d", expr->as.literal_expr.value);
    break;

  case EXPR_FLOAT_LITERAL:
    EMIT("%.17g", expr->as.float_expr.value);
    break;

  case EXPR_STRING:
    EMIT("\"%.*s\"", (int)expr->as.string_expr.length,
         expr->as.string_expr.value);
    break;

  case EXPR_ARRAY_LITERAL: {
    char typeName[256];
    c_name_for_type(expr->type, typeName, sizeof typeName);
    EMIT("(%s){ .data = { ", typeName);
    bool first = true;
    for (ExprList *el = expr->as.array_literal_expr.elements; el; el = el->next) {
        if (!first) EMIT(", ");
        first = false;
        emit_expr(el->expr, depth);
    }
    EMIT(" } }");
    break;
  }

  case EXPR_CHAR: {
    unsigned char v = expr->as.char_expr.value;
    switch (v) {
    case '\n':
      EMIT("'\\n'");
      break;
    case '\r':
      EMIT("'\\r'");
      break;
    case '\t':
      EMIT("'\\t'");
      break;
    case '\\':
      EMIT("'\\\\'");
      break;
    case '\'':
      EMIT("'\\\''");
      break;
    default:
      if (v < 32 || v > 126) {
        EMIT("'\\x%02X'", v);
      } else {
        EMIT("'%c'", v);
      }
    }
    break;
  }

  case EXPR_IDENTIFIER: {
    // var T primitive param in value context: the C representation is T*, so we
    // must dereference to get/write the value. Structs are exempt because they
    // are always accessed through EXPR_MEMBER which already emits '->'.
    bool is_var_prim_param = expr->decl &&
        expr->decl->kind == DECL_VARIABLE &&
        expr->decl->as.variable_decl.is_parameter &&
        expr->decl->as.variable_decl.type &&
        expr->decl->as.variable_decl.type->mode == MODE_MUTABLE &&
        is_primitive_type(expr->decl->as.variable_decl.type);
    if (is_var_prim_param) {
        EMIT("(*%s)", c_name_for_id(expr->as.identifier_expr.id));
    } else {
        EMIT("%s", c_name_for_id(expr->as.identifier_expr.id));
    }
    break;
  }

  case EXPR_BINARY: {
    // Special‑case: slice == string literal → length check + memcmp
    if (expr->as.binary_expr.op == TOKEN_EQUAL &&
        expr->as.binary_expr.left->type &&
        expr->as.binary_expr.left->type->kind == TYPE_SLICE &&
        expr->as.binary_expr.right->kind == EXPR_STRING) {
      Expr *slice = expr->as.binary_expr.left;
      Expr *lit = expr->as.binary_expr.right;
      int L = (int)lit->as.string_expr.length;
      const char *S = lit->as.string_expr.value;

      EMIT("(");
      // check length
      emit_expr(slice, 0);
      EMIT(".len == %d && ", L);
      // compare data via memcmp
      EMIT("memcmp(");
      emit_expr(slice, 0);
      EMIT(".data, \"%.*s\", %d) == 0)", L, S, L);
    } else if (expr->as.binary_expr.op == TOKEN_KEYWORD_AND) {
      // logical AND → emit "(<left> && <right>)"
      EMIT("(");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(" && ");
      emit_expr(expr->as.binary_expr.right, depth);
      EMIT(")");
    } else if (expr->as.binary_expr.op == TOKEN_KEYWORD_OR) {
      // logical OR → emit "(<left> || <right>)"
      EMIT("(");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(" || ");
      emit_expr(expr->as.binary_expr.right, depth);
      EMIT(")");
    } else if (expr->as.binary_expr.op == TOKEN_KEYWORD_IN) {
      // idx in arr → (idx >= 0 && idx < arr.len)
      Type *ct = expr->as.binary_expr.right->type;
      if (ct) ct = sema_unwrap_type(ct);
      EMIT("(");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(" >= 0 && ");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(" < ");
      if (ct && ct->kind == TYPE_ARRAY && ct->array_len >= 0) {
        EMIT("%lld", (long long)ct->array_len);
      } else {
        // Dynamic slice param: length is the hidden __len_NAME parameter.
        Expr *rhs = expr->as.binary_expr.right;
        if (rhs->kind == EXPR_IDENTIFIER && rhs->as.identifier_expr.id) {
          Id *rname = rhs->as.identifier_expr.id;
          EMIT("__len_%.*s", (int)rname->length, rname->name);
        } else {
          emit_expr(rhs, depth);
          EMIT(".len");
        }
      }
      EMIT(")");
    } else if (expr->as.binary_expr.op == TOKEN_PLUS_PERCENT
            || expr->as.binary_expr.op == TOKEN_MINUS_PERCENT
            || expr->as.binary_expr.op == TOKEN_ASTERISK_PERCENT) {
      // Q-002 wrapping ops: emit plain C op. C semantics on unsigned types are
      // already wrapping; on signed types we rely on -fwrapv at compile time.
      const char *op_c = (expr->as.binary_expr.op == TOKEN_PLUS_PERCENT)  ? "+"
                       : (expr->as.binary_expr.op == TOKEN_MINUS_PERCENT) ? "-"
                                                                          : "*";
      EMIT("(");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(" %s ", op_c);
      emit_expr(expr->as.binary_expr.right, depth);
      EMIT(")");
    } else if (expr->as.binary_expr.op == TOKEN_PLUS_PIPE
            || expr->as.binary_expr.op == TOKEN_MINUS_PIPE
            || expr->as.binary_expr.op == TOKEN_ASTERISK_PIPE) {
      // Q-002 saturating ops: compute in a wider type then clamp to the
      // declared range. For signed iN: int64_t intermediate. For unsigned uN:
      // also int64_t (signed) so the < 0 case is representable.
      const char *op_c = (expr->as.binary_expr.op == TOKEN_PLUS_PIPE)  ? "+"
                       : (expr->as.binary_expr.op == TOKEN_MINUS_PIPE) ? "-"
                                                                       : "*";
      Type *lt = expr->as.binary_expr.left ? expr->as.binary_expr.left->type : NULL;
      long long lo = -2147483648LL, hi = 2147483647LL;
      if (lt && lt->kind == TYPE_SIMPLE && lt->base_type) {
        int bits; bool sgn;
        if (parse_iN_uN(lt, &bits, &sgn)) {
          if (!sgn) {
            lo = 0;
            hi = (bits >= 63) ? 9223372036854775807LL : ((1LL << bits) - 1);
          } else {
            long long b = bits;
            lo = -(1LL << (b - 1));
            hi =  (1LL << (b - 1)) - 1;
          }
        }
      }
      // Build: ({ int64_t __t = (int64_t)L op (int64_t)R; __t > hi ? hi : (__t < lo ? lo : __t); })
      // We avoid statement expressions for portability and inline a ternary.
      // Pattern: (((int64_t)L op (int64_t)R) > hi) ? hi : ( ((int64_t)L op (int64_t)R) < lo ? lo : ((int64_t)L op (int64_t)R) )
      EMIT("( ");
      // outer: > hi check
      EMIT("(((int64_t)(");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(") %s (int64_t)(", op_c);
      emit_expr(expr->as.binary_expr.right, depth);
      EMIT(")) > %lldLL) ? %lldLL : ", hi, hi);
      // < lo check
      EMIT("( (((int64_t)(");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(") %s (int64_t)(", op_c);
      emit_expr(expr->as.binary_expr.right, depth);
      EMIT(")) < %lldLL) ? %lldLL : ", lo, lo);
      // in-range: emit the actual op
      EMIT("((int64_t)(");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(") %s (int64_t)(", op_c);
      emit_expr(expr->as.binary_expr.right, depth);
      EMIT(")) )");
      EMIT(" )");
    } else {
      // Fallback for all other binary ops: +, -, *, /, %, <, ==, bitwise, etc.
      EMIT("(");
      emit_expr(expr->as.binary_expr.left, depth);
      EMIT(" %s ", token_kind_to_str(expr->as.binary_expr.op));
      emit_expr(expr->as.binary_expr.right, depth);
      EMIT(")");
    }
    break;
  }

  case EXPR_UNARY:
    EMIT("%s", token_kind_to_str(expr->as.unary_expr.op));
    emit_expr(expr->as.unary_expr.right, depth);
    break;

  case EXPR_MEMBER: {
    ExprMember *m = &expr->as.member_expr;

    // Sprint 19: packed struct field access → StructName_get_field(r)
    if (m->target && m->target->type
        && m->target->type->kind == TYPE_SIMPLE
        && m->target->type->base_type) {
      char tnam[256];
      isize tl = m->target->type->base_type->length;
      if (tl < (isize)sizeof(tnam)) {
        memcpy(tnam, m->target->type->base_type->name, tl);
        tnam[tl] = '\0';
        extern Symbol *sema_lookup(const char *name);
        Symbol *tsym = sema_lookup(tnam);
        if (tsym && tsym->decl && tsym->decl->kind == DECL_STRUCT
            && tsym->decl->as.struct_decl.is_packed) {
          const char *sn = c_name_for_id(tsym->decl->as.struct_decl.name);
          EMIT("%s_get_%.*s(", sn, (int)m->member->length, m->member->name);
          emit_expr(m->target, 0);
          EMIT(")");
          break;
        }
      }
    }

    // Check if this is an ADT variant access (e.g. Shape.Point) used as a value
    if (m->target->kind == EXPR_IDENTIFIER || m->target->kind == EXPR_TYPE) {
        // We rely on the fact that sema_infer_expr sets the type to the ADT type
        // But we need to know if it's a type name.
        // Let's check if the resolved decl is an Enum.
        Decl *d = m->target->decl;
        if (d && (d->kind == DECL_ENUM || d->kind == DECL_STRUCT)) {
             const char *raw_adt_name = NULL;
             if (m->target->kind == EXPR_IDENTIFIER) {
                 raw_adt_name = c_name_for_id(m->target->as.identifier_expr.id);
             } else if (m->target->kind == EXPR_TYPE && m->target->as.type_expr.type_value && m->target->as.type_expr.type_value->kind == TYPE_SIMPLE) {
                 raw_adt_name = c_name_for_id(m->target->as.type_expr.type_value->base_type);
             }
             if (raw_adt_name) {
                 char adt_name[256];
                 strncpy(adt_name, raw_adt_name, sizeof(adt_name));
                 adt_name[sizeof(adt_name)-1] = '\0';
                 
                 const char *variant_name = c_name_for_id(m->member);
                 // Emit as a constructor call: Shape_Point()
                 EMIT("%s_%s()", adt_name, variant_name);
                 break;
             }
        }
    }

    bool is_ptr = false;
    Type *t = m->target->type;
    if (t) {
        // ... (existing logic) ...
        if (t->kind == TYPE_POINTER) {
            is_ptr = true;
        }
        else if (t->mode == MODE_MUTABLE) {
            is_ptr = true;
        }
        else if (t->mode == MODE_OWNED) {
            is_ptr = false;
        }
        else if (t->mode == MODE_SHARED) {
            if (t->kind == TYPE_SIMPLE || t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) {
                if (m->target->kind == EXPR_IDENTIFIER) {
                    Decl *d = m->target->decl;
                    if (d && d->kind == DECL_VARIABLE) {
                        if (d->as.variable_decl.is_parameter && !is_primitive_type(t)) {
                            is_ptr = true;
                        }
                    }
                }
            }
        }
    }

    if (expr->type && expr->type->kind == TYPE_VARIANT) {
        emit_expr(m->target, depth);
        EMIT(is_ptr ? "->data.%.*s" : ".data.%.*s", (int)m->member->length, m->member->name);
        break;
    }

    // Fase 7: dynamic array param.len → __len_PARAM (or size_expr for constrained params)
    if (m->member->length == 3 && strncmp(m->member->name, "len", 3) == 0 &&
        m->target->kind == EXPR_IDENTIFIER && m->target->decl &&
        is_dynarray_param_decl(m->target->decl)) {
        Type *tgt_t = m->target->decl->as.variable_decl.type;
        if (tgt_t->size_expr == NULL) {
            Id *pname = m->target->decl->as.variable_decl.name;
            EMIT("__len_%.*s", (int)pname->length, pname->name);
        } else {
            // Constrained param: emit size_expr without sema_infer_expr
            // (type-annotation nodes are unresolved; use raw-name emitter)
            emit_size_expr(tgt_t->size_expr, depth);
        }
        break;
    }

    // Fase 7: dynamic array param.data → the raw C pointer (it IS the data)
    if (m->member->length == 4 && strncmp(m->member->name, "data", 4) == 0 &&
        m->target->kind == EXPR_IDENTIFIER && m->target->decl &&
        is_dynarray_param_decl(m->target->decl)) {
        emit_expr(m->target, depth); // just emit the identifier; the C pointer IS the data
        break;
    }

    emit_expr(m->target, depth);
    EMIT(is_ptr ? "->%.*s" : ".%.*s", (int)m->member->length, m->member->name);
    break;
  }

  case EXPR_CALL: {
    // Q-014/G-007: panic builtin — emit as fprintf+abort
    // S16: panic = abort puro (no defer, no unwind)
    if (expr->as.call_expr.callee->kind == EXPR_IDENTIFIER) {
      Id *cid = expr->as.call_expr.callee->as.identifier_expr.id;
      if (cid->length == 5 && strncmp(cid->name, "panic", 5) == 0) {
        Expr *arg = expr->as.call_expr.args ? expr->as.call_expr.args->expr : NULL;
        if (arg && arg->kind == EXPR_STRING) {
          // Direct string literal: emit as plain C string
          EMIT("(fprintf(stderr, \"panic: %.*s\\n\"), abort(), 0)",
               (int)arg->as.string_expr.length, arg->as.string_expr.value);
        } else {
          // Generic slice: msg.data and msg.len
          EMIT("(fprintf(stderr, \"panic: %%.*s\\n\", (int)(");
          if (arg) emit_expr(arg, 0);
          else EMIT("(u8[]){0}");
          EMIT(").len, (");
          if (arg) emit_expr(arg, 0);
          else EMIT("(u8[]){0}");
          EMIT(").data), abort(), 0)");
        }
        break;
      }
    }

    // 1) figure out the C‐name of the callee
    const char *cname = NULL;
    if (expr->as.call_expr.callee->kind == EXPR_IDENTIFIER) {
      Id *id = expr->as.call_expr.callee->as.identifier_expr.id;
      cname = c_name_for_id(id);
    } else if (expr->as.call_expr.callee->kind == EXPR_TYPE) {
      if (expr->as.call_expr.callee->decl && expr->as.call_expr.callee->decl->kind == DECL_STRUCT) {
          cname = c_name_for_id(expr->as.call_expr.callee->decl->as.struct_decl.name);
      }
    }
  
    // 2) detect struct‐ctor
    bool is_ctor = cname && is_struct_type(cname);
  
    // 3) find the matching DeclStruct
    DeclStruct *sd = NULL;
    if (is_ctor) {
      // Use strrchr (last '_') so module-prefixed names like
      // "tests_foo_bar_MyStruct" → "MyStruct" (not "foo_bar_MyStruct")
      const char *unders = strrchr(cname, '_');
      const char *struct_name = unders ? unders + 1 : cname;
      for (DeclList *dl = emitted_decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (d->kind == DECL_STRUCT &&
            strncmp(d->as.struct_decl.name->name, struct_name,
                    d->as.struct_decl.name->length) == 0) {
          sd = &d->as.struct_decl;
          break;
        }
      }
    }
  
    // 4) emit the function/ctor name
    if (is_ctor) {
      EMIT("%s_ctor", cname);
    } else if (cname) {
      EMIT("%s", cname);
    } else if (expr->as.call_expr.callee->kind == EXPR_MEMBER) {
        // Check for ADT constructor: Shape.Circle(...)
        Expr *target = expr->as.call_expr.callee->as.member_expr.target;
        if (target->kind == EXPR_IDENTIFIER || target->kind == EXPR_TYPE) {
            const char *raw_adt_name = NULL;
            if (target->kind == EXPR_IDENTIFIER) {
                raw_adt_name = c_name_for_id(target->as.identifier_expr.id);
            } else if (target->kind == EXPR_TYPE && target->as.type_expr.type_value && target->as.type_expr.type_value->kind == TYPE_SIMPLE) {
                raw_adt_name = c_name_for_id(target->as.type_expr.type_value->base_type);
            }
            if (raw_adt_name) {
                char adt_name[256];
                strncpy(adt_name, raw_adt_name, sizeof(adt_name));
                adt_name[sizeof(adt_name)-1] = '\0';
                
                const char *variant_name = c_name_for_id(expr->as.call_expr.callee->as.member_expr.member);
                // fprintf(stderr, "DEBUG: EXPR_CALL ADT: adt='%s', var='%s'\n", adt_name, variant_name);
                EMIT("%s_%s", adt_name, variant_name);
            } else {
                emit_expr(expr->as.call_expr.callee, depth);
            }
        } else {
            emit_expr(expr->as.call_expr.callee, depth);
        }
    } else {
      emit_expr(expr->as.call_expr.callee, depth);
    }
  
    // 5) emit the argument list
    EMIT("(");
    bool first = true;
    DeclList *fld = sd ? sd->fields : NULL;
    
    // Lookup function parameters if it's a regular function call
    DeclList *param = NULL;
    if (!is_ctor) {
    if (!is_ctor) {
         // Attempt 1: Use sema-resolved declaration if available (Robust)
         Expr *callee = expr->as.call_expr.callee;
         if (callee->kind == EXPR_IDENTIFIER) {
             if (callee->decl) {
                 // fprintf(stderr, "DEBUG: callee->decl found! Kind=%d\n", callee->decl->kind);
                 if (callee->decl->kind == DECL_FUNCTION) {
                     param = callee->decl->as.function_decl.params;
                 } else if (callee->decl->kind == DECL_PROCEDURE) {
                     // Procedure shares mostly same layout, but let's be safe
                     param = callee->decl->as.function_decl.params;
                 }
             } else {
                 // callee->decl is NULL (e.g. libc functions) — fall through to other lookups
             }
         }
         
         // Attempt 3: Fallback to global symbol table lookup (sema_lookup)
         // This handles cross-module lookups where callee->decl might reference a Symbol?
         if (!param && cname) {
             extern Symbol *sema_lookup(const char *name); // Forward declare
             Symbol *sym = sema_lookup(cname);
             if (sym && sym->decl && sym->decl->kind == DECL_FUNCTION) {
                 param = sym->decl->as.function_decl.params;
             } else if (sym && sym->decl && sym->decl->kind == DECL_PROCEDURE) {
                 param = sym->decl->as.function_decl.params;
             } else {
                 // sema_lookup failed or not function (normal for c interop calls often not in symbol table)
             }
         }
         
         // Attempt 4: Fallback to string lookup in emitted_decls (Legacy)
         if (!param && cname) {
             DeclFunction *fd = lookup_function_decl(expr->as.call_expr.callee);
             if (fd) param = fd->params;
         }
    }
    }

    for (ExprList *arg = expr->as.call_expr.args; arg; arg = arg->next) {
      if (arg->expr->kind == EXPR_TYPE) {
          if (fld) fld = fld->next;
          if (param) param = param->next;
          continue;
      }
      if (!first) EMIT(", ");
      first = false;
      if (is_ctor && fld) {
        // ... (existing ctor code) ...
        Type *ft = fld->decl->as.variable_decl.type;
        // SPECIAL-CASE: string literal into fixed-length field (array or slice-with-fixed-len)
        if ((ft->kind == TYPE_ARRAY && ft->array_len >= 0)
            || (ft->kind == TYPE_SLICE && ft->sentinel_str == NULL && ft->sentinel_len > 0)) {
          Expr *lit = arg->expr;
          // Check if it's actually a string literal before accessing as.string_expr
          if (lit->kind == EXPR_STRING) {
              size_t fixed_len = (ft->kind == TYPE_ARRAY) ? (size_t)ft->array_len : (size_t)ft->sentinel_len;
              int L = (int)lit->as.string_expr.length;
              const unsigned char *S = (const unsigned char*)lit->as.string_expr.value;

              // Native C array initializer: { 0x78, ... }
              EMIT("{ ");
              for (size_t i = 0; i < fixed_len; i++) {
                unsigned v = (i < (size_t)L) ? (unsigned)S[i] : 0u;
                EMIT("0x%02X", v);
                if (i + 1 < fixed_len) EMIT(", ");
              }
              EMIT(" }");

              fld = fld->next;
              if (param) param = param->next; // Advance param too if it exists
              continue;
          }
        }

        // Dynamic slice param → Slice_u8 field: reconstruct from decomposed (len, ptr)
        if (ft->kind == TYPE_ARRAY && ft->array_len == -1 &&
            arg->expr->kind == EXPR_IDENTIFIER && arg->expr->decl &&
            is_dynarray_param_decl(arg->expr->decl)) {
            Id *an = arg->expr->as.identifier_expr.id;
            char sliceBuf[256];
            c_name_for_type(ft, sliceBuf, sizeof sliceBuf);
            EMIT("(%s){ .len = __len_%.*s, .data = %.*s }",
                 sliceBuf, (int)an->length, an->name,
                 (int)an->length, an->name);
            fld = fld->next;
            if (param) param = param->next;
            continue;
        }

        // Auto-coerce: fixed array u8[N] → dynamic/sentinel slice in struct ctor
        if (emit_slice_coercion(ft, arg->expr, depth)) {
            fld = fld->next;
            if (param) param = param->next;
            continue;
        }

        // ORIGINAL: sentinel-terminated slice literal (keep existing behavior)
        if (ft->kind == TYPE_SLICE && arg->expr->kind == EXPR_STRING) {
           // your previous sentinel-handling logic:
           Expr *lit = arg->expr;
           int L = (int)lit->as.string_expr.length;
           const unsigned char *S = (const unsigned char*)lit->as.string_expr.value;
           char sliceBuf[256];
           c_name_for_type(ft, sliceBuf, sizeof sliceBuf);

           EMIT("(%s){ .data = (uint8_t[]){ ", sliceBuf);
           for (int i = 0; i < L; i++) {
             EMIT("0x%02X, ", S[i]);
           }
           EMIT("0 } }"); // explicit sentinel byte appended
           fld = fld->next;
           if (param) param = param->next; // Advance param too if it exists
           continue;
        }
      }
      
      // Handle implicit address-of for shared/mutable reference parameters
      if (param) {
           Type *pt = param->decl->as.variable_decl.type;

           // Auto-decay: u8[N] → *u8[N] (thin pointer).
           // u8[N] is now a native C array — it decays to a pointer automatically.
           if (pt && pt->kind == TYPE_POINTER &&
               pt->element_type && pt->element_type->kind == TYPE_ARRAY &&
               pt->element_type->array_len >= 0) {
               Type *at = arg->expr->type ? sema_unwrap_type(arg->expr->type) : NULL;
               if (!at || (at->kind == TYPE_ARRAY && at->array_len >= 0)) {
                   emit_expr(arg->expr, depth);
                   // Native C arrays decay to pointer in C — no .data or & needed
                   goto next_arg;
               }
           }

           // Fase 7: dynamic array param at call site → (size_t len,)? T *data
           if (param->decl->kind == DECL_VARIABLE &&
               pt && pt->kind == TYPE_ARRAY && pt->array_len == -1) {
               Expr *base_arg = arg->expr;
               if (base_arg->kind == EXPR_MUT) base_arg = base_arg->as.mut_expr.expr;
               Type *at = base_arg->type ? sema_unwrap_type(base_arg->type) : NULL;
               // Inject length iff the callee param has no size_expr
               if (pt->size_expr == NULL) {
                   if (at && at->kind == TYPE_ARRAY && at->array_len >= 0) {
                       EMIT("(size_t)%lld, ", (long long)at->array_len);
                   } else if (at && at->kind == TYPE_ARRAY && at->array_len == -1 &&
                              base_arg->kind == EXPR_IDENTIFIER && base_arg->decl) {
                       Id *an = base_arg->decl->as.variable_decl.name;
                       EMIT("__len_%.*s, ", (int)an->length, an->name);
                   } else {
                       emit_expr(base_arg, depth); EMIT(".len, ");
                   }
               }
               // Emit data pointer
               if (at && at->kind == TYPE_ARRAY && at->array_len >= 0) {
                   emit_expr(base_arg, depth);
                   // Native C array — decays to pointer, no .data field.
               } else if (at && at->kind == TYPE_ARRAY && at->array_len == -1) {
                   emit_expr(base_arg, depth);
                   // Decomposed dynarray param: already a raw pointer, no .data.
                   // Slice struct field (e.g. l.text): needs .data to get the pointer.
                   bool _is_decomposed = (base_arg->kind == EXPR_IDENTIFIER && base_arg->decl &&
                                          is_dynarray_param_decl(base_arg->decl));
                   if (!_is_decomposed) EMIT(".data");
               } else {
                   emit_expr(base_arg, depth); EMIT(".data");
               }
               goto next_arg;
           }

           // Attempt coercion for sentinel slices (e.g. "foo" -> u8[:0] or fixed var)
           if (emit_slice_coercion(pt, arg->expr, depth)) {
               goto next_arg;
           }

           // Check if we need to pass by pointer (implicit reference)
           bool needs_ptr = false;
           
           if (pt->mode == MODE_MUTABLE) {
               needs_ptr = true;
           } else if (pt->mode == MODE_SHARED && !is_primitive_type(pt)) {
               needs_ptr = true;
           }

           if (needs_ptr) {
               Type *at = arg->expr->type;
               if (at && at->mode != MODE_MUTABLE && at->kind != TYPE_POINTER) {
                   EMIT("&(");
                   emit_expr(arg->expr, depth);
                   EMIT(")");
                   goto next_arg;
               }
               // Argument is already a pointer (var T param forwarded to another var T param).
               // Emit the raw identifier to pass the pointer as-is — do not auto-deref.
               if (at && at->mode == MODE_MUTABLE && arg->expr->kind == EXPR_IDENTIFIER) {
                   EMIT("%s", c_name_for_id(arg->expr->as.identifier_expr.id));
                   goto next_arg;
               }
           }
      }
  
      // fallback for everything else
      emit_expr(arg->expr, depth);
      
      next_arg:
      if (fld) fld = fld->next;
      if (param) param = param->next;
    }
    EMIT(")");
    break;
  }
  
  
  
  

  case EXPR_INDEX: {
    ExprIndex *ix = &expr->as.index_expr;

    // Range‐slice: foo[a..b] or foo[a..]
    if (ix->index->kind == EXPR_RANGE) {
      ExprRange *r = &ix->index->as.range_expr;

      // We know sema has set expr->type to the correct slice type
      // (including sentinel variant if any), so emit that.
      char sliceBuf[256];
      c_name_for_type(expr->type, sliceBuf, sizeof sliceBuf);

      // Open the struct literal with the real slice type
      EMIT("(%s){ .data = ", sliceBuf);

      // pointer = original slice .data + start index
      emit_expr(ix->target, 0);
      EMIT(".data + ");
      emit_expr(r->start, 0);

      // length = end - start (+1 if inclusive)
      EMIT(", .len = ");
      emit_expr(r->end, 0);
      EMIT(" - ");
      emit_expr(r->start, 0);
      if (r->inclusive) {
        EMIT(" + 1");
      }

      EMIT(" }");
    } else {
      // Plain indexing: T = data[i]
      
      // Check if target is a pointer (e.g. shared param)
      bool is_ptr = false;
      if (ix->target->decl && ix->target->decl->kind == DECL_VARIABLE) {
          Type *t = ix->target->decl->as.variable_decl.type;
          // Logic matching emit_decl.h: shared structs/arrays are const pointers
          // mutable are pointers
          // owned are value
          if (t && !is_primitive_type(t)) {
               if (t->mode == MODE_SHARED || t->mode == MODE_MUTABLE) {
                   is_ptr = true;
               }
          }
      }
      // Also check explicit pointer types
      if (ix->target->type && (ix->target->type->kind == TYPE_POINTER)) {
          is_ptr = true;
      }
    
      // Fase 7: dynamic array params are raw pointers — index directly
      if (ix->target->kind == EXPR_IDENTIFIER && ix->target->decl &&
          is_dynarray_param_decl(ix->target->decl)) {
          emit_expr(ix->target, 0);
          EMIT("[");
          emit_expr(ix->index, 0);
          EMIT("]");
      } else {
          emit_expr(ix->target, 0);
          // User-type fixed arrays and thin pointers (*T[N]): index directly
          bool is_thin_ptr = ix->target->type && ix->target->type->kind == TYPE_POINTER &&
                             ix->target->type->element_type &&
                             ix->target->type->element_type->kind == TYPE_ARRAY;
          if (is_user_type_fixed_array(ix->target->type) || is_thin_ptr) {
              EMIT("[");
          } else {
              EMIT(is_ptr ? "->data[" : ".data[");
          }
          emit_expr(ix->index, 0);
          EMIT("]");
      }
    }
    break;
  }

  case EXPR_MOVE:
    emit_expr(expr->as.move_expr.expr, depth);
    break;

  case EXPR_MUT: {
    // When the inner expression is already a `var T` parameter (which
    // is stored as T* in C), passing `&inner` would create a double
    // pointer. Detect that case and emit the identifier directly.
    Expr *inner = expr->as.mut_expr.expr;
    bool already_pointer = false;
    if (inner && inner->kind == EXPR_IDENTIFIER && inner->decl &&
        inner->decl->kind == DECL_VARIABLE &&
        inner->decl->as.variable_decl.is_parameter &&
        inner->decl->as.variable_decl.type &&
        inner->decl->as.variable_decl.type->mode == MODE_MUTABLE) {
        already_pointer = true;
    }
    if (already_pointer) {
        // Emit the raw pointer identifier — do NOT go through emit_expr, which
        // would auto-deref var primitive params and produce (*r) instead of r.
        EMIT("%s", c_name_for_id(inner->as.identifier_expr.id));
    } else {
        EMIT("&(");
        emit_expr(inner, depth);
        EMIT(")");
    }
    break;
  }

  case EXPR_CAST: {
    char tybuf[256];
    c_name_for_type(expr->as.cast_expr.target_type, tybuf, sizeof tybuf);
    EMIT("((%s)(", tybuf);
    emit_expr(expr->as.cast_expr.expr, depth);
    EMIT("))");
    break;
  }

  case EXPR_MATCH: {
    char scrut_c_ty[256];
    c_name_for_type(expr->as.match_expr.value->type, scrut_c_ty, sizeof(scrut_c_ty));

    char res_c_ty[256];
    c_name_for_type(expr->type, res_c_ty, sizeof(res_c_ty));

    static int __expr_match_cnt = 0;
    int __match_id = __expr_match_cnt++;

    // D-Niche M4: locate the ADT decl (if any) and its niche layout.
    Decl *xmatch_adt = NULL;
    NicheLayout xmatch_niche = {0};
    bool xmatch_use_niche = false;
    if (expr->as.match_expr.value->type &&
        expr->as.match_expr.value->type->kind == TYPE_SIMPLE) {
        const char *tname = c_name_for_id(
            expr->as.match_expr.value->type->base_type);
        size_t tlen = strlen(tname);
        for (DeclList *dl = sema_decls; dl; dl = dl->next) {
            if (!dl->decl || dl->decl->kind != DECL_ENUM) continue;
            Id *en = dl->decl->as.enum_decl.type_name;
            if (!en) continue;
            if (((size_t)en->length == tlen &&
                 strncmp(en->name, tname, tlen) == 0) ||
                (tlen > (size_t)en->length + 1 &&
                 tname[tlen - en->length - 1] == '_' &&
                 strncmp(tname + tlen - en->length, en->name, en->length) == 0)) {
                xmatch_adt = dl->decl;
                break;
            }
        }
        if (xmatch_adt) {
            xmatch_use_niche = enum_is_zero_cost_niche(xmatch_adt, &xmatch_niche);
        }
    }
    
    EMIT("({\n");
    emit_indent(depth + 1);
    EMIT("%s __match%d = ", scrut_c_ty, __match_id);
    // Dereference by-pointer parameters (const T* or T*)
    {
        Expr *scrut_expr = expr->as.match_expr.value;
        bool needs_deref = false;
        if (scrut_expr->kind == EXPR_IDENTIFIER) {
            Decl *d = scrut_expr->decl;
            if (d && d->kind == DECL_VARIABLE && d->as.variable_decl.is_parameter) {
                Type *t = d->as.variable_decl.type;
                if (t) {
                    if (t->mode == MODE_MUTABLE) {
                        needs_deref = true;
                    } else if (!is_primitive_type(t)) {
                        needs_deref = true;
                    }
                }
            }
        }
        if (needs_deref) EMIT("*");
        emit_expr(scrut_expr, depth + 1);
    }
    EMIT(";\n");
    
    emit_indent(depth + 1);
    EMIT("%s __result%d;\n", res_c_ty, __match_id);

    Type *scrut_type = sema_unwrap_type(expr->as.match_expr.value->type);
    
    bool first_clause = true;
    for (ExprMatchCase *c = expr->as.match_expr.cases; c; c = c->next) {
        emit_indent(depth + 1);
        if (c->patterns) {
            EMIT(first_clause ? "if (" : "else if (");
            bool first_cond = true;
            for (ExprList *p = c->patterns; p; p = p->next) {
                if (!first_cond) EMIT(" || ");
                first_cond = false;

                switch (p->expr->kind) {
                  case EXPR_STRING: {
                    int L = (int)p->expr->as.string_expr.length;
                    const unsigned char *S = (const unsigned char*)p->expr->as.string_expr.value;
                    if (L == 1 && scrut_type && scrut_type->kind == TYPE_SIMPLE) {
                      EMIT("__match%d == '%c'", __match_id, S[0]);
                    } else {
                      EMIT("__match%d.len == %d && "
                           "memcmp(__match%d.data, \"%.*s\", %d) == 0",
                           __match_id, L, __match_id, L, S, L);
                    }
                    break;
                   }
                  case EXPR_CHAR: {
                    EMIT("__match%d == ", __match_id);
                    emit_expr(p->expr, depth);
                    break;
                  }
                  case EXPR_RANGE: {
                    char lo = p->expr->as.range_expr.start->as.char_expr.value;
                    char hi = p->expr->as.range_expr.end->as.char_expr.value;
                    EMIT("(__match%d >= '%c' && __match%d <= '%c')", __match_id, lo,
                         __match_id, hi);
                    break;
                  }
                  case EXPR_CALL: {
                      if (xmatch_use_niche && xmatch_adt) {
                          // Niche path: comparison against the payload
                          // variant's exclusion of all empty sentinels.
                          // (Pattern is Variant(bindings) → assume payload.)
                          EMIT("(1");
                          for (Variant *ev = xmatch_adt->as.enum_decl.variants; ev; ev = ev->next) {
                              if (ev->fields) continue;
                              long long sv = niche_sentinel_for_variant(
                                  &xmatch_adt->as.enum_decl, ev, &xmatch_niche);
                              if (xmatch_niche.pool.kind == POOL_POINTER) {
                                  EMIT(" && (uintptr_t)__match%d != %lldULL",
                                       __match_id, sv);
                              } else {
                                  EMIT(" && __match%d != %lldLL",
                                       __match_id, sv);
                              }
                          }
                          EMIT(")");
                      } else {
                          EMIT("(__match%d.tag == ", __match_id);
                          emit_expr(p->expr->as.call_expr.callee, depth + 1);
                          EMIT(")");
                      }
                      break;
                  }
                  case EXPR_IDENTIFIER: {
                      if (p->expr->is_global && p->expr->decl && p->expr->decl->kind == DECL_ENUM) {
                          // Strip ADT prefix from mangled variant id.
                          char adt_buf[256];
                          strncpy(adt_buf, c_name_for_id(p->expr->decl->as.enum_decl.type_name), sizeof(adt_buf));
                          adt_buf[sizeof(adt_buf)-1] = '\0';
                          size_t adt_len = strlen(adt_buf);
                          Id *vid = p->expr->as.identifier_expr.id;
                          const char *raw_variant = vid->name;
                          int raw_len = (int)vid->length;
                          if (raw_len > (int)adt_len + 1 &&
                              strncmp(vid->name, adt_buf, adt_len) == 0 &&
                              vid->name[adt_len] == '_') {
                              raw_variant = vid->name + adt_len + 1;
                              raw_len = raw_len - (int)adt_len - 1;
                          }
                          // Niche path: compare scrutinee against the
                          // variant's assigned sentinel value.
                          if (xmatch_use_niche && xmatch_adt) {
                              Variant *matched = NULL;
                              for (Variant *v = xmatch_adt->as.enum_decl.variants; v; v = v->next) {
                                  if ((int)v->name->length == raw_len &&
                                      strncmp(v->name->name, raw_variant, raw_len) == 0) {
                                      matched = v; break;
                                  }
                              }
                              if (matched) {
                                  long long sv = niche_sentinel_for_variant(
                                      &xmatch_adt->as.enum_decl, matched, &xmatch_niche);
                                  if (xmatch_niche.pool.kind == POOL_POINTER) {
                                      EMIT("(uintptr_t)__match%d == %lldULL",
                                           __match_id, sv);
                                  } else {
                                      EMIT("__match%d == %lldLL",
                                           __match_id, sv);
                                  }
                                  break;
                              }
                          }
                          EMIT("__match%d.tag == %s_Tag_%.*s", __match_id, adt_buf,
                               raw_len, raw_variant);
                          break;
                      }
                      EMIT("__match%d == ", __match_id);
                      emit_expr(p->expr, depth);
                      break;
                  }
                  default: {
                    EMIT("__match%d == ", __match_id);
                    emit_expr(p->expr, depth);
                    break;
                  }
                }
            }
            EMIT(") {\n");
        } else {
            EMIT(first_clause ? "if (1) {\n" : "else {\n");
        }
        
        emit_indent(depth + 2);
        EMIT("__result%d = ", __match_id);
        // Coerce string literal to slice type in case expression arms.
        // Use (uint8_t*)"str" instead of compound literal to avoid
        // dangling pointer from block-scoped compound literals in ({...}).
        if (c->body->kind == EXPR_STRING && expr->type &&
            expr->type->kind == TYPE_SLICE) {
            int L = (int)c->body->as.string_expr.length;
            const char *S = c->body->as.string_expr.value;
            EMIT("(%s){ .len = %d, .data = (uint8_t*)\"%.*s\" }", res_c_ty, L, L, S);
        } else {
            emit_expr(c->body, depth + 2);
        }
        EMIT(";\n");
        
        emit_indent(depth + 1);
        EMIT("}\n");
        first_clause = false;
    }
    
    emit_indent(depth + 1);
    EMIT("__result%d;\n", __match_id);
    emit_indent(depth);
    EMIT("})");
    break;
  }

  default:
    EMIT("/* unhandled expression type */");
    break;
  }
}

#endif // EMIT_EXPR_H
