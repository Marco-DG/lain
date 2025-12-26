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

  case EXPR_STRING:
    EMIT("\"%.*s\"", (int)expr->as.string_expr.length,
         expr->as.string_expr.value);
    break;

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

  case EXPR_IDENTIFIER:
    EMIT("%s", c_name_for_id(expr->as.identifier_expr.id));
    break;

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
    
    bool is_ptr = false;
    Type *t = m->target->type;
    if (t) {
        // Check for explicit pointer type
        if (t->kind == TYPE_POINTER) {
            is_ptr = true;
        }
        // MODE_MUTABLE is always a pointer (T*)
        else if (t->mode == MODE_MUTABLE) {
            is_ptr = true;
        }
        // MODE_OWNED is always a value (T) - use .
        else if (t->mode == MODE_OWNED) {
            is_ptr = false;
        }
        // MODE_SHARED: check if it's a parameter (passed as const T*)
        else if (t->mode == MODE_SHARED) {
            if (t->kind == TYPE_SIMPLE || t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) {
                // Check if target is a parameter (Shared Reference -> const T*)
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

    emit_expr(m->target, depth);
    EMIT(is_ptr ? "->%.*s" : ".%.*s", (int)m->member->length, m->member->name);
    break;
  }

  case EXPR_CALL: {
    // 1) figure out the C‐name of the callee
    const char *cname = NULL;
    if (expr->as.call_expr.callee->kind == EXPR_IDENTIFIER) {
      Id *id = expr->as.call_expr.callee->as.identifier_expr.id;
      cname = c_name_for_id(id);
    }
    //fprintf(stderr, "[debug] EXPR_CALL: cname='%s'\n", cname ?: "<anon>");
  
    // 2) detect struct‐ctor
    bool is_ctor = cname && is_struct_type(cname);
    //fprintf(stderr, "[debug]   is_ctor? %s\n", is_ctor ? "yes":"no");
  
    // 3) find the matching DeclStruct
    DeclStruct *sd = NULL;
    if (is_ctor) {
      const char *unders = strchr(cname, '_');
      const char *struct_name = unders ? unders + 1 : cname;
      //fprintf(stderr, "[debug]   struct_name='%s'\n", struct_name);
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
    //fprintf(stderr, "[debug]   found DeclStruct? %s\n", sd ? "yes":"no");
  
    // 4) emit the function/ctor name
    if (is_ctor) {
      EMIT("%s_ctor", cname);
      //fprintf(stderr, "[debug]   using ctor '%s_ctor'\n", cname);
    } else if (cname) {
      EMIT("%s", cname);
      //fprintf(stderr, "[debug]   plain call '%s'\n", cname);
    } else {
      emit_expr(expr->as.call_expr.callee, depth);
      //fprintf(stderr, "[debug]   anon callee fallback\n");
    }
  
    // 5) emit the argument list
    EMIT("(");
    bool first = true;
    DeclList *fld = sd ? sd->fields : NULL;
    
    // Lookup function parameters if it's a regular function call
    DeclList *param = NULL;
    if (!is_ctor && cname) {
         DeclFunction *fd = lookup_function_decl(expr->as.call_expr.callee);
         if (fd) {
             param = fd->params;
         }
    }

    for (ExprList *arg = expr->as.call_expr.args; arg; arg = arg->next) {
      if (!first) EMIT(", ");
      first = false;
  
      //fprintf(stderr,
      //        "[debug]   arg kind=%d  have-next-field? %s\n",
      //        arg->expr->kind,
      //        fld ? "yes":"no");
  
      if (is_ctor && fld) {
        // what is this field’s type?
        Type *ft = fld->decl->as.variable_decl.type;
        //fprintf(stderr,
        //        "[debug]     field '%.*s' type kind=%d\n",
        //        (int)fld->decl->as.variable_decl.name->length,
        //        fld->decl->as.variable_decl.name->name,
        //        ft->kind);
  
        
        // SPECIAL-CASE: string literal into fixed-length field (array or slice-with-fixed-len)
        if ((ft->kind == TYPE_ARRAY && ft->array_len >= 0)
            || (ft->kind == TYPE_SLICE && ft->sentinel_str == NULL && ft->sentinel_len > 0)) {
          Expr *lit = arg->expr;
          // Check if it's actually a string literal before accessing as.string_expr
          if (lit->kind == EXPR_STRING) {
              size_t fixed_len = (ft->kind == TYPE_ARRAY) ? (size_t)ft->array_len : (size_t)ft->sentinel_len;
              int L = (int)lit->as.string_expr.length;
              const unsigned char *S = (const unsigned char*)lit->as.string_expr.value;

              char sliceBuf[64];
              c_name_for_type(ft, sliceBuf, sizeof sliceBuf);

              // emit inline array literal with EXACT fixed_len bytes (no trailing NUL)
              EMIT("(%s){ .data = (uint8_t[]){ ", sliceBuf);
              for (size_t i = 0; i < fixed_len; i++) {
                unsigned v = (i < (size_t)L) ? (unsigned)S[i] : 0u;
                EMIT("0x%02X", v);
                if (i + 1 < fixed_len) EMIT(", ");
              }
              EMIT(" } }");

              fld = fld->next;
              if (param) param = param->next; // Advance param too if it exists
              continue;
          }
        }

        // ORIGINAL: sentinel-terminated slice literal (keep existing behavior)
        if (ft->kind == TYPE_SLICE && arg->expr->kind == EXPR_STRING) {
           // your previous sentinel-handling logic:
           Expr *lit = arg->expr;
           int L = (int)lit->as.string_expr.length;
           const unsigned char *S = (const unsigned char*)lit->as.string_expr.value;
           char sliceBuf[64];
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
      
      // Handle implicit address-of for shared reference parameters
      if (param) {
           Type *pt = param->decl->as.variable_decl.type;
           // If parameter is Shared Reference (not mut, not mov) AND not primitive
           if (pt->mode == MODE_SHARED && !is_primitive_type(pt)) {
               // Expects const T*
               Type *at = arg->expr->type;
               // If argument is passed by value (not mut, not pointer), emit &
               if (at && at->mode != MODE_MUTABLE && at->kind != TYPE_POINTER) {
                   EMIT("&(");
                   emit_expr(arg->expr, depth);
                   EMIT(")");
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
      char sliceBuf[64];
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
      emit_expr(ix->target, 0);
      EMIT(".data[");
      emit_expr(ix->index, 0);
      EMIT("]");
    }
    break;
  }

  case EXPR_MOVE:
    emit_expr(expr->as.move_expr.expr, depth);
    break;

  case EXPR_MUT:
    EMIT("&(");
    emit_expr(expr->as.mut_expr.expr, depth);
    EMIT(")");
    break;

  default:
    EMIT("/* unhandled expression type */");
    break;
  }
}

#endif // EMIT_EXPR_H
