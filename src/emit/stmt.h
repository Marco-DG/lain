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
  
        if (!emit_slice_coercion(ty, rhs, depth)) {
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
    if (it->kind == EXPR_RANGE) {
        is_range = true;
        r = &it->as.range_expr;
    } else if (it->kind == EXPR_INDEX) {
      ix = &it->as.index_expr;
      if (ix->index->kind == EXPR_RANGE) {
        is_range = true;
        r = &ix->index->as.range_expr;
      }
    }

    // 3) hoist into __sliceN (ONLY if it's a slice iteration)
    if (is_range && ix) {
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

    if (is_range && !ix) {
        // Direct range loop: for (int i = start; i < end; ++i)
        EMIT("for (int %s = ", __i_var);
        emit_expr(r->start, 0);
        EMIT("; %s < ", __i_var);
        emit_expr(r->end, 0);
        if (r->inclusive) {
             EMIT(" + 1");
        }
        EMIT("; ++%s) {\n", __i_var);
    } else {
        // Standard 0-based index loop
        EMIT("for (size_t %s = 0; ", __i_var);

        if (is_range) {
          if (r->end) {
            // bounded slice → iterate by length
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
    }

    // 5) bind index var if present
    if (stmt->as.for_stmt.index_name) {
      emit_indent(depth + 1);
      EMIT("size_t %.*s = %s;\n", (int)stmt->as.for_stmt.index_name->length,
           stmt->as.for_stmt.index_name->name, __i_var);
    }

    // 6) bind value var
    emit_indent(depth + 1);
    if (is_range && !ix) {
        // For range loop, value is the index itself (cast to int if needed)
        EMIT("int %.*s = (int)%s;\n", (int)stmt->as.for_stmt.value_name->length,
             stmt->as.for_stmt.value_name->name, __i_var);
    } else {
        EMIT("int %.*s = %s.data[%s];\n", (int)stmt->as.for_stmt.value_name->length,
             stmt->as.for_stmt.value_name->name, __slice_var, __i_var);
    }

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

  case STMT_BREAK:
    emit_indent(depth);
    EMIT("break;\n");
    break;

  case STMT_WHILE: {
    // 1) emit "while (<cond>) {"
    emit_indent(depth);
    EMIT("while (");
    emit_expr(stmt->as.while_stmt.cond, depth);
    EMIT(") {\n");

    // 2) emit body
    emit_stmt_list(stmt->as.while_stmt.body, depth + 1);

    // 3) close
    emit_indent(depth);
    EMIT("}\n");
    break;
  }

  case STMT_MATCH: {
    // 1) figure out the scrutinee’s C type (handles simple, array, slice,…)
    Expr *scrut = stmt->as.match_stmt.value;
    sema_resolve_expr(scrut);
    sema_infer_expr(scrut);
    char c_ty[128];
    c_name_for_type(scrut->type, c_ty, sizeof c_ty);

    // Check if it is an ADT
    bool is_adt = false;
    Decl *adt_decl = NULL;
    char adt_cname[256];
    if (scrut->type && scrut->type->kind == TYPE_SIMPLE) {
        // We need the resolved name of the type for C code generation
        const char *tname = c_name_for_id(scrut->type->base_type);
        strncpy(adt_cname, tname, sizeof(adt_cname));
        adt_cname[sizeof(adt_cname)-1] = '\0';
        
        // Find the Decl by iterating sema_decls and checking for suffix match
        // tname is likely mangled (tests_adt_Shape). Decl name is raw (Shape).
        size_t tlen = strlen(tname);
        
        for (DeclList *dl = sema_decls; dl; dl = dl->next) {
            if (!dl->decl || dl->decl->kind != DECL_ENUM) continue;
            Id *enum_name = dl->decl->as.enum_decl.type_name;
            if (!enum_name) continue;
            
            // 1. Exact match
            if ((size_t)enum_name->length == tlen &&
                strncmp(enum_name->name, tname, tlen) == 0) {
                is_adt = true;
                adt_decl = dl->decl;
                break;
            }
            
            // 2. Suffix match
            if (tlen > (size_t)enum_name->length + 1) {
                const char *suffix = tname + (tlen - enum_name->length);
                if (*(suffix-1) == '_' && strncmp(suffix, enum_name->name, enum_name->length) == 0) {
                    is_adt = true;
                    adt_decl = dl->decl;
                    break;
                }
            }
        }
    }

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
        
        if (is_adt) {
            // ADT Pattern Matching
            // Check tag: __matchN.tag == ADT_Tag_Variant
            for (StmtMatchCase *p = c;; p = p->next) {
                Id *variant_id = NULL;
                if (p->pattern->kind == EXPR_CALL) {
                    // Variant(...)
                    Expr *callee = p->pattern->as.call_expr.callee;
                    if (callee->kind == EXPR_IDENTIFIER) variant_id = callee->as.identifier_expr.id;
                    else if (callee->kind == EXPR_MEMBER) variant_id = callee->as.member_expr.member;
                } else if (p->pattern->kind == EXPR_IDENTIFIER) {
                    // Variant
                    variant_id = p->pattern->as.identifier_expr.id;
                }
                
                if (variant_id) {
                    // We need the raw variant name to construct the Tag name
                    // The variant_id might be mangled or not.
                    // But we know the ADT C name.
                    // The tag is ADT_Tag_VariantRawName.
                    // We need to find the Variant in the ADT Decl to get the raw name.
                    // Or we can try to extract it from variant_id?
                    // Better to lookup in ADT Decl.
                    
                    // const char *search_name = variant_id->name; // Unused
                    // If mangled (tests_adt_Shape_Circle), extract suffix?
                    // Or just iterate ADT variants and match?
                    
                    Variant *matched_v = NULL;
                    for (Variant *v = adt_decl->as.enum_decl.variants; v; v = v->next) {
                        // Check exact or suffix match
                        if (v->name->length == variant_id->length &&
                            strncmp(v->name->name, variant_id->name, v->name->length) == 0) {
                            matched_v = v; break;
                        }
                        // Suffix match
                        if (variant_id->length > v->name->length + 1) {
                             const char *suffix = variant_id->name + (variant_id->length - v->name->length);
                             if (*(suffix-1) == '_' && strncmp(suffix, v->name->name, v->name->length) == 0) {
                                 matched_v = v; break;
                             }
                        }
                    }
                    
                    if (matched_v) {
                        EMIT("__match%d.tag == %s_Tag_%.*s", __match_id, adt_cname, (int)matched_v->name->length, matched_v->name->name);
                    } else {
                        EMIT("0 /* unknown variant */");
                    }
                } else {
                    EMIT("0 /* invalid pattern */");
                }

                if (p == group) break;
                EMIT(" || ");
            }
        } else {
            // Existing logic for non-ADT
            for (StmtMatchCase *p = c;; p = p->next) {
              switch (p->pattern->kind) {
              case EXPR_STRING: {
                int L = (int)p->pattern->as.string_expr.length;
                const unsigned char *S = (const unsigned char*)p->pattern->as.string_expr.value;
                if (L == 1 && scrut->type && scrut->type->kind == TYPE_SIMPLE) {
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
                EMIT("__match%d == ", __match_id);
                emit_expr(p->pattern, depth);
                break;
              }
              }
              if (p == group)
                break;
              EMIT(" || ");
            }
        }
        EMIT(") ");
      } else {
        // catch‐all
        EMIT(first_clause ? "if (1) " : "else ");
      }

      // 4) body
      EMIT("{\n");
      
      // Emit bindings for ADT patterns
      if (is_adt && group->pattern && group->pattern->kind == EXPR_CALL) {
          // Destructuring: Variant(a, b)
          // We need to match arguments to fields.
          // We need the Variant Decl again.
          // (Repeating lookup - could be optimized)
          Id *variant_id = NULL;
          Expr *callee = group->pattern->as.call_expr.callee;
          if (callee->kind == EXPR_IDENTIFIER) variant_id = callee->as.identifier_expr.id;
          else if (callee->kind == EXPR_MEMBER) variant_id = callee->as.member_expr.member;
          
          Variant *matched_v = NULL;
          if (variant_id) {
              for (Variant *v = adt_decl->as.enum_decl.variants; v; v = v->next) {
                    if (v->name->length == variant_id->length &&
                        strncmp(v->name->name, variant_id->name, v->name->length) == 0) {
                        matched_v = v; break;
                    }
                    if (variant_id->length > v->name->length + 1) {
                         const char *suffix = variant_id->name + (variant_id->length - v->name->length);
                         if (*(suffix-1) == '_' && strncmp(suffix, v->name->name, v->name->length) == 0) {
                             matched_v = v; break;
                         }
                    }
              }
          }
          
          if (matched_v) {
              ExprList *arg = group->pattern->as.call_expr.args;
              DeclList *field = matched_v->fields;
              while (arg && field) {
                  if (arg->expr->kind == EXPR_IDENTIFIER) {
                      Id *var_name = arg->expr->as.identifier_expr.id;
                      // Emit: Type var = __matchN.data.Variant.field;
                      // We need the field type.
                      Type *ft = field->decl->as.variable_decl.type;
                      char fty[128];
                      c_name_for_type(ft, fty, sizeof fty);
                      
                      emit_indent(depth + 1);
                      EMIT("%s %.*s = __match%d.data.%.*s.%.*s;\n",
                           fty,
                           (int)var_name->length, var_name->name,
                           __match_id,
                           (int)matched_v->name->length, matched_v->name->name,
                           (int)field->decl->as.variable_decl.name->length, field->decl->as.variable_decl.name->name);
                  }
                  arg = arg->next;
                  field = field->next;
              }
          }
      }

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

  case STMT_UNSAFE: {
    emit_indent(depth);
    EMIT("/* unsafe block */\n");
    emit_indent(depth);
    EMIT("{\n");
    
    // Set unsafe context so lazy-inference in emit_expr knows we are safe
    bool old_unsafe = sema_in_unsafe_block;
    sema_in_unsafe_block = true;
    
    emit_stmt_list(stmt->as.unsafe_stmt.body, depth + 1);
    
    sema_in_unsafe_block = old_unsafe;
    
    emit_indent(depth);
    EMIT("}\n");
    break;
  }
  
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
        if (!emit_slice_coercion(ty, rhs, depth)) {
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
