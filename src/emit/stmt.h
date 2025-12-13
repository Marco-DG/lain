#ifndef EMIT_STMT_H
#define EMIT_STMT_H

#include "../emit.h"
#include "core.h"

// Emit a single statement
void emit_stmt(Stmt *stmt, int depth);
// Emit a list of statements (e.g. function body)
void emit_stmt_list(StmtList *stmts, int depth);

void emit_stmt(Stmt *stmt, int depth) {
  if (!stmt)
    return;
  // Skip pure‐compile‐time `use` statements (they don't emit any code),
  // and avoid eating an indent with no newline.
  if (stmt->kind == STMT_USE) {
    return;
  }
  switch (stmt->kind) {
    case STMT_VAR: {
      // 1) indent
      emit_indent(depth);
  
      // 2) emit the C type (or default to int)
      if (stmt->as.var_stmt.type) {
        char tybuf[128];
        c_name_for_type(stmt->as.var_stmt.type, tybuf, sizeof tybuf);
        EMIT("%s", tybuf);
      } else {
        EMIT("int");
      }
  
      // 3) emit the variable name
      Id *v = stmt->as.var_stmt.name;
      EMIT(" %s", c_name_for_id(v));
  
      // 4) optional initializer
      if (stmt->as.var_stmt.expr) {
        EMIT(" = ");
  
        // centralized helper: emits compound byte array literal for fixed-like types
        Type *ty = stmt->as.var_stmt.type;
        Expr *rhs = stmt->as.var_stmt.expr;
  
        if (!emit_fixed_string_init(ty, rhs, depth)) {
          // fallback to general expression emission
          emit_expr(rhs, depth);
        }
      }
  
      // 5) terminate
      EMIT(";\n");
      break;
    }
  

  case STMT_FOR: {
    // 1) pick unique names
    emit_indent(depth);
    static int __for_cnt = 0;
    char __i_var[32], __slice_var[32];
    snprintf(__i_var, sizeof __i_var, "__i%d", __for_cnt);
    snprintf(__slice_var, sizeof __slice_var, "__slice%d", __for_cnt);
    __for_cnt++;

    // 2) check if iterable is a range‐slice
    Expr *it = stmt->as.for_stmt.iterable;
    bool is_range = false;
    ExprIndex *ix = NULL;
    ExprRange *r = NULL;
    if (it->kind == EXPR_INDEX) {
      ix = &it->as.index_expr;
      if (ix->index->kind == EXPR_RANGE) {
        is_range = true;
        r = &ix->index->as.range_expr;
      }
    }

    // 3) hoist into __sliceN
    if (is_range) {
      const char *sliceName;
      if (r->end) {
        // bounded [a..b] → length‐based slice
        Type *elem_ty = it->type->element_type;
        Type *len_slice = type_array(sema_arena, elem_ty, -1);
        sliceName = emit_slice_type_definition(len_slice);
        EMIT("%s %s = ", sliceName, __slice_var);
        emit_expr(it, 0); // emits (Slice_u8){ .data, .len }
      } else {
        // open [a..] → zero‐sentinel slice of the original field
        Type *orig_slice_ty = (Type*)ix->target->type;
        sliceName = emit_slice_type_definition(orig_slice_ty); 
               
        EMIT("%s %s = (%s){ .data = ", sliceName, __slice_var, sliceName);
        emit_expr(ix->target, 0);
        if (orig_slice_ty->kind == TYPE_SLICE) {
          EMIT(".data");
        }
        EMIT(" + ");
        emit_expr(r->start, 0);
        EMIT(" }");
      }
      EMIT(";\n");
    }

    // 4) emit for‐loop header
    emit_indent(depth);
    EMIT("for (size_t %s = 0; ", __i_var);

    if (is_range) {
      if (r->end) {
        // bounded → iterate by length
        EMIT("%s < %s.len; ++%s) {\n", __i_var, __slice_var, __i_var);
      } else {
        // open → sentinel test on zero‐sentinel slice
        Type *orig_slice_ty = (Type*)ix->target->type;
        const char *origName = emit_slice_type_definition(orig_slice_ty);
        
        EMIT("%s.data[%s] != %s_SENTINEL; ++%s) {\n", __slice_var, __i_var,
             origName, __i_var);
      }
    } else if (it->type && it->type->kind == TYPE_SLICE) {
      // plain sentinel slice
      const char *plainName = emit_slice_type_definition(it->type);
      EMIT("%s.data[%s] != %s_SENTINEL; ++%s) {\n", __slice_var, __i_var,
           plainName, __i_var);
    } else {
      // fixed‐length array
      EMIT("%s < ", __i_var);
      emit_expr(it, 0);
      EMIT(".len; ++%s) {\n", __i_var);
    }

    // 5) bind index var if present
    if (stmt->as.for_stmt.index_name) {
      emit_indent(depth + 1);
      EMIT("size_t %.*s = %s;\n", (int)stmt->as.for_stmt.index_name->length,
           stmt->as.for_stmt.index_name->name, __i_var);
    }

    // 6) bind value var
    emit_indent(depth + 1);
    EMIT("int %.*s = %s.data[%s];\n", (int)stmt->as.for_stmt.value_name->length,
         stmt->as.for_stmt.value_name->name, __slice_var, __i_var);

    // 7) loop body
    emit_stmt_list(stmt->as.for_stmt.body, depth + 1);

    // 8) close
    emit_indent(depth);
    EMIT("}\n");
    break;
  }

  case STMT_IF: {
    // 1) extract condition, then‐branch, else‐branch
    Expr *cond = stmt->as.if_stmt.cond;
    StmtList *then_pl = stmt->as.if_stmt.then_branch;
    StmtList *else_pl = stmt->as.if_stmt.else_branch;

    // 2) emit "if (<cond>) {"
    emit_indent(depth);
    EMIT("if (");
    emit_expr(cond, depth);
    EMIT(") {\n");

    // 3) emit the `then` block
    emit_stmt_list(then_pl, depth + 1);

    // 4) close the `then` block
    emit_indent(depth);
    EMIT("}");

    // 5) if there's an `else` branch, decide between "else if" vs. "else"
    if (else_pl) {
      // check if else_pl is exactly one statement and that statement is itself
      // an `if`
      if (else_pl->next == NULL && else_pl->stmt->kind == STMT_IF) {
        // "else if (…)" : simply emit a space and recursively emit that STMT_IF
        EMIT(" else ");
        // NOTE: we call emit_stmt on the nested STMT_IF with the same `depth`
        //       so that it emits "if (…) { … }" without adding a newline
        //       before.
        emit_stmt(else_pl->stmt, depth);
      } else {
        // plain "else"
        EMIT(" else {\n");
        emit_stmt_list(else_pl, depth + 1);
        emit_indent(depth);
        EMIT("}\n");
      }
    } else {
      // no else: just emit newline
      EMIT("\n");
    }
    break;
  }

  case STMT_CONTINUE:
    emit_indent(depth);
    EMIT("continue;\n");
    break;

  case STMT_MATCH: {
    // 1) figure out the scrutinee’s C type (handles simple, array, slice,…)
    Expr *scrut = stmt->as.match_stmt.value;
    sema_resolve_expr(scrut);
    sema_infer_expr(scrut);
    char c_ty[128];
    c_name_for_type(scrut->type, c_ty, sizeof c_ty);

    // 2) bind to __matchN with the real C type
    emit_indent(depth);
    static int __match_cnt = 0;
    int __match_id = __match_cnt++;
    EMIT("%s __match%d = ", c_ty, __match_id);
    emit_expr(scrut, depth);
    EMIT(";\n");

    // 3) each case
    bool first_clause = true;
    for (StmtMatchCase *c = stmt->as.match_stmt.cases; c; /**/) {
      // group fall‐through cases
      StmtMatchCase *group = c;
      while (group && !group->body && group->next)
        group = group->next;

      emit_indent(depth);
      // start if / else if / else
      if (group->pattern) {
        EMIT(first_clause ? "if (" : "else if (");
        // emit each sub‐pattern in this group
        for (StmtMatchCase *p = c;; p = p->next) {
          switch (p->pattern->kind) {
          case EXPR_STRING: {
            // if it's a 1-byte literal on a simple (char/int) scrutinee, emit a char match;
            // otherwise do the normal slice+memcmp
            int L = (int)p->pattern->as.string_expr.length;
            const unsigned char *S = (const unsigned char*)p->pattern->as.string_expr.value;
            if (L == 1 && scrut->type && scrut->type->kind == TYPE_SIMPLE) {
              // single‐char string → char literal match
              EMIT("__match%d == '%c'", __match_id, S[0]);
            } else {
              // generic slice/string literal → len + memcmp
              EMIT("__match%d.len == %d && "
                   "memcmp(__match%d.data, \"%.*s\", %d) == 0",
                   __match_id, L, __match_id, L, S, L);
            }
            break;
           }
          case EXPR_CHAR: {
            EMIT("__match%d == ", __match_id);
            emit_expr(p->pattern, depth);
            break;
          }
          case EXPR_RANGE: {
            char lo = p->pattern->as.range_expr.start->as.char_expr.value;
            char hi = p->pattern->as.range_expr.end->as.char_expr.value;
            EMIT("(__match%d >= '%c' && __match%d <= '%c')", __match_id, lo,
                 __match_id, hi);
            break;
          }
          default: {
            // identifier or numeric literal
            EMIT("__match%d == ", __match_id);
            emit_expr(p->pattern, depth);
            break;
          }
          }
          if (p == group)
            break;
          EMIT(" || ");
        }
        EMIT(") ");
      } else {
        // catch‐all
        EMIT(first_clause ? "if (1) " : "else ");
      }

      // 4) body
      EMIT("{\n");
      if (group->body) {
        emit_stmt_list(group->body, depth + 1);
      }
      emit_indent(depth);
      EMIT("}\n");

      first_clause = false;
      c = group->next;
    }
    break;
  }

  case STMT_RETURN:
    emit_indent(depth);
    EMIT("return ");
    emit_expr(stmt->as.return_stmt.value, depth);
    EMIT(";\n");
    break;

    case STMT_ASSIGN: {
      Expr *lhs = stmt->as.assign_stmt.target;
      Expr *rhs = stmt->as.assign_stmt.expr;
  
      // only turn bare identifiers *without* a dot in their C‐name into
      // `const T x = ...;`.  Anything else (including a "member" name
      // like "lexer.cursor") becomes a plain assignment.
      if ( stmt->as.assign_stmt.is_const
        && lhs->kind == EXPR_IDENTIFIER
        && !strchr(c_name_for_id(lhs->as.identifier_expr.id), '.') )
      {
        // 1) indent to the current depth
        emit_indent(depth);
  
        // 2) pick up the type
        Type *ty = rhs->type ? rhs->type : get_builtin_int_type();
        char tybuf[128];
        c_name_for_type(ty, tybuf, sizeof tybuf);
  
        // 3) name
        Id *id = lhs->as.identifier_expr.id;
        EMIT("const %s %s = ", tybuf, c_name_for_id(id));
  
        // 4) centralized helper for fixed-length string init (or fallback)
        if (!emit_fixed_string_init(ty, rhs, depth)) {
            // fallback to whatever the expression prints
            emit_expr(rhs, depth);
        }
  
        // 5) newline
        EMIT(";\n");
      } else {
        // Normal assignment with indent
        emit_indent(depth);
        emit_expr(lhs, depth);
        EMIT(" = ");
        emit_expr(rhs, depth);
        EMIT(";\n");
      }
      break;
    }
  

  case STMT_EXPR:
    // Emit expression statement (e.g. a function call)
    emit_indent(depth);
    emit_expr(stmt->as.expr_stmt.expr, depth);
    EMIT(";\n");
    break;
  default:
    emit_indent(depth);
    EMIT("/* unhandled statement type */\n");
    break;
  }
}

void emit_stmt_list(StmtList *stmt_list, int depth) {
  while (stmt_list) {
    emit_stmt(stmt_list->stmt, depth);
    stmt_list = stmt_list->next;
  }
}

#endif // EMIT_STMT_H
