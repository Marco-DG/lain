#ifndef SEMA_TYPECHECK_H
#define SEMA_TYPECHECK_H

#include "../ast.h"
#include "resolve.h"
#include "bounds.h"  // Static bounds checking
#include <assert.h>
#include <string.h>

/* Forward decls used elsewhere */
Type *get_builtin_int_type(void);
Type *get_builtin_u8_type(void);
void sema_infer_expr(Expr *e);
void sema_resolve_expr(Expr *e); // forward

#include "ranges.h" // Range analysis

extern Arena *sema_arena;
extern DeclList *sema_decls;
extern Type *current_return_type;
extern Decl *current_function_decl; // Defined in sema.h
extern RangeTable *sema_ranges;     // Defined in sema.h

// ...



// ...



/*─────────────────────────────────────────────────────────────────╗
│ 1) Helpers to get a builtin “int” Type* only once               │
╚─────────────────────────────────────────────────────────────────*/

Type *get_builtin_int_type(void) {
  static Type *int_ty = NULL;
  if (!int_ty) {
    // make a fake Id for “int” in the arena
    Id *id = arena_push_aligned(sema_arena, Id);
    id->name = "int";
    id->length = 3;
    int_ty = type_simple(sema_arena, id);
  }
  return int_ty;
}

Type *get_builtin_u8_type(void) {
  static Type *u8_ty = NULL;
  if (!u8_ty) {
    // make a fake Id for “u8”
    Id *id = arena_push_aligned(sema_arena, Id);
    id->name = "u8";
    id->length = 2;
    u8_ty = type_simple(sema_arena, id);
  }
  return u8_ty;
}

/*─────────────────────────────────────────────────────────────────╗
│ 2) Keep the top-level DeclList for struct lookups              │
╚─────────────────────────────────────────────────────────────────*/

/* lookup a struct Decl node by its name Id */
static DeclStruct *find_struct_decl(Id *struct_name) {
  if (!struct_name)
    return NULL;
  for (DeclList *dl = sema_decls; dl; dl = dl->next) {
    Decl *d = dl->decl;
    if (d && d->kind == DECL_STRUCT &&
        d->as.struct_decl.name->length == struct_name->length &&
        strncmp(d->as.struct_decl.name->name, struct_name->name,
                struct_name->length) == 0) {
      return &d->as.struct_decl;
    }
  }
  return NULL;
}

/* lookup a field’s Type* given a struct and field Id */
static Type *lookup_struct_field_type(Id *struct_name, Id *field) {
  if (!struct_name) {
    fprintf(stderr, "sema error: internal: lookup_struct_field_type called "
                    "with NULL struct_name\n");
    exit(1);
  }

  DeclStruct *sd = find_struct_decl(struct_name);
  if (!sd) {
    fprintf(stderr, "sema error: unknown struct ‘%.*s’\n",
            (int)struct_name->length, struct_name->name);
    exit(1);
  }
  for (DeclList *fld = sd->fields; fld; fld = fld->next) {
    Decl *vd = fld->decl;
    if (vd->kind == DECL_VARIABLE) {
      Id *fname = vd->as.variable_decl.name;
      if (fname->length == field->length &&
          strncmp(fname->name, field->name, fname->length) == 0) {
        return vd->as.variable_decl.type;
      }
    }
  }
  fprintf(stderr, "sema error: struct ‘%.*s’ has no field ‘%.*s’\n",
          (int)struct_name->length, struct_name->name, (int)field->length,
          field->name);
  exit(1);
}

/*
    type inference/checking logic
*/

/* Unwrap wrapper types to get the underlying type (struct/array/slice) */
static Type *sema_unwrap_type(Type *t) {
    while (t) {
        // With the new OwnershipMode system, we only unwrap pointer/comptime
        // The mode is just a field on the type, not a wrapper
        if (t->kind == TYPE_POINTER) t = t->element_type;
        else if (t->kind == TYPE_COMPTIME) t = t->element_type;
        else break;
    }
    return t;
}



/* lookup an ADT Decl node by its name Id */
static DeclEnum *find_adt_decl(Id *adt_name) {
  if (!adt_name) return NULL;
  for (DeclList *dl = sema_decls; dl; dl = dl->next) {
    Decl *d = dl->decl;
    if (d && d->kind == DECL_ENUM &&
        d->as.enum_decl.type_name->length == adt_name->length &&
        strncmp(d->as.enum_decl.type_name->name, adt_name->name, adt_name->length) == 0) {
      return &d->as.enum_decl;
    }
  }
  return NULL;
}

/* lookup a variant in an ADT */
static Variant *lookup_adt_variant(DeclEnum *adt, Id *variant_name) {
    for (Variant *v = adt->variants; v; v = v->next) {
        if (v->name->length == variant_name->length &&
            strncmp(v->name->name, variant_name->name, variant_name->length) == 0) {
            return v;
        }
    }
    return NULL;
}

void sema_infer_expr(Expr *e) {
  if (!e) return;
  // fprintf(stderr, "DEBUG: sema_infer_expr called for kind %d\n", e->kind);
  switch (e->kind) {
  case EXPR_IDENTIFIER:
    // already set in resolve
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        if (e->is_global && e->decl && e->decl->kind == DECL_VARIABLE && e->decl->as.variable_decl.is_mutable) {
            fprintf(stderr, "sema error: pure function '%.*s' cannot read mutable global variable '%.*s'\n",
                    (int)current_function_decl->as.function_decl.name->length, current_function_decl->as.function_decl.name->name,
                    (int)e->as.identifier_expr.id->length, e->as.identifier_expr.id->name);
            exit(1);
        }
    }
    break;

// ...

  case EXPR_MEMBER: {
    sema_infer_expr(e->as.member_expr.target);
    Type *t = e->as.member_expr.target->type;
    
    // Case 1: Accessing ADT Variant Constructor (e.g. Shape.Circle)
    // Case 1: Accessing ADT Variant Constructor (e.g. Shape.Circle)
    if (e->as.member_expr.target->kind == EXPR_IDENTIFIER) {
        Id *base_name = e->as.member_expr.target->as.identifier_expr.id;
        DeclEnum *adt = NULL;
        
        // Try to use resolved decl first
        if (e->as.member_expr.target->decl && e->as.member_expr.target->decl->kind == DECL_ENUM) {
            adt = &e->as.member_expr.target->decl->as.enum_decl;
        } else {
            // Fallback to name lookup (though resolve should have handled it)
            adt = find_adt_decl(base_name);
        }

        if (adt) {
            // It is an ADT type name!
            Variant *v = lookup_adt_variant(adt, e->as.member_expr.member);
            if (!v) {
                fprintf(stderr, "sema error: ADT '%.*s' has no variant '%.*s'\n",
                        (int)base_name->length, base_name->name,
                        (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
                exit(1);
            }
            // The type of "Shape.Circle" is the ADT type itself (conceptually)
            // But we need to know it's a constructor.
            // For now, let's just set the type to the ADT type.
            // We need to construct a Type for the ADT.
            // Since we don't have a "TypeDecl" pointer in Type, we reconstruct it.
            // TODO: Cache this or store Decl in Type.
            Type *adt_type = type_simple(sema_arena, base_name);
            e->type = adt_type;
            
            // Tag the expression as a variant constructor for code gen
            // We can reuse `decl` field to point to the ADT decl? No, that's for variables.
            // We might need a new ExprKind or just rely on context.
            // Let's rely on EXPR_CALL handling this if it's a call.
            // If it's just `Shape.Point` (no args), it's a value.
            return;
        }
    }

    assert(t && "member on untyped target");
    
    // Unwrap wrappers (mut, mov, ptr)
    t = sema_unwrap_type(t);

    // If target is a slice or array, handle the common fields: .len and .data
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) {
      Id *mem = e->as.member_expr.member;
      if (mem && mem->length == 3 && strncmp(mem->name, "len", 3) == 0) {
        // .len → integer
        e->type = get_builtin_int_type();
        break;
      }
      if (mem && mem->length == 4 && strncmp(mem->name, "data", 4) == 0) {
        // .data → pointer to element_type
        e->type = type_pointer(sema_arena, t->element_type);
        break;
      }
    }

    // fall back to struct field lookup (existing behavior)
    e->type = lookup_struct_field_type(t->base_type, e->as.member_expr.member);
    break;
  }

// ...

  case EXPR_CALL: {
    // ensure callee resolved & infer args
    sema_infer_expr(e->as.call_expr.callee); // Changed from resolve to infer to handle Shape.Circle
    
    // Check if this is an ADT constructor call
    if (e->as.call_expr.callee->kind == EXPR_MEMBER) {
        Expr *target = e->as.call_expr.callee->as.member_expr.target;
        if (target->kind == EXPR_IDENTIFIER) {
             DeclEnum *adt = NULL;
             if (target->decl && target->decl->kind == DECL_ENUM) {
                 adt = &target->decl->as.enum_decl;
             } else {
                 adt = find_adt_decl(target->as.identifier_expr.id);
             }
             
             if (adt) {
                 // It IS an ADT constructor call: Shape.Circle(...)
                 // Verify arguments match fields
                 Variant *v = lookup_adt_variant(adt, e->as.call_expr.callee->as.member_expr.member);
                 assert(v && "Variant should have been found in EXPR_MEMBER");
                 
                 ExprList *arg = e->as.call_expr.args;
                 DeclList *field = v->fields;
                 
                 int arg_idx = 0;
                 while (arg && field) {
                     sema_infer_expr(arg->expr);
                     // TODO: Check type compatibility between arg->expr->type and field->decl->type
                     arg = arg->next;
                     field = field->next;
                     arg_idx++;
                 }
                 
                 if (arg || field) {
                     fprintf(stderr, "sema error: wrong number of arguments for variant constructor '%.*s'\n",
                             (int)v->name->length, v->name->name);
                     exit(1);
                 }
                 
                 e->type = e->as.call_expr.callee->type; // The ADT type
                 break;
             }
        }
    }
    
    // Normal function call logic...
    sema_resolve_expr(e->as.call_expr.callee);
    
    // Purity check: func cannot call proc
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        Expr *callee = e->as.call_expr.callee;
        if (callee->decl) {
            if (callee->decl->kind == DECL_PROCEDURE || callee->decl->kind == DECL_EXTERN_PROCEDURE) {
                fprintf(stderr, "sema error: pure function '%.*s' cannot call procedure\n",
                        (int)current_function_decl->as.function_decl.name->length, current_function_decl->as.function_decl.name->name);
                exit(1);
            }
            
            // Termination Analysis: Ban recursion in func
            if (callee->decl == current_function_decl) {
                fprintf(stderr, "sema error: recursion is not allowed in pure function '%.*s' (to guarantee termination)\n",
                        (int)current_function_decl->as.function_decl.name->length, current_function_decl->as.function_decl.name->name);
                exit(1);
            }
        }
    }

    for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
      sema_infer_expr(a->expr);
    }
    
    // Verify Pre-Contracts
    if (e->as.call_expr.callee->decl && e->as.call_expr.callee->decl->kind == DECL_FUNCTION) {
        Decl *callee_decl = e->as.call_expr.callee->decl;
        if (callee_decl->as.function_decl.pre_contracts) {
            for (ExprList *pre = callee_decl->as.function_decl.pre_contracts; pre; pre = pre->next) {
                if (pre->expr->kind == EXPR_BINARY) {
                    Expr *lhs = pre->expr->as.binary_expr.left;
                    Expr *rhs = pre->expr->as.binary_expr.right;
                    TokenKind op = pre->expr->as.binary_expr.op;
                    
                    if (lhs->kind == EXPR_IDENTIFIER && rhs->kind == EXPR_LITERAL) {
                        // ... existing literal logic ...
                        // (Keep existing logic for literal, or replace it all? Let's keep it for now but maybe clean up later)
                        // Actually, let's use the generic approach for everything if possible.
                    }
                    
                    // Generic approach: Substitute params with args and check
                    // Find param indices for lhs and rhs (if they are identifiers)
                    Expr *arg_lhs = NULL;
                    Expr *arg_rhs = NULL;
                    
                    if (lhs->kind == EXPR_IDENTIFIER) {
                        int p_idx = 0;
                        for (DeclList *p = callee_decl->as.function_decl.params; p; p = p->next) {
                            if (p->decl->as.variable_decl.name->length == lhs->as.identifier_expr.id->length &&
                                strncmp(p->decl->as.variable_decl.name->name, lhs->as.identifier_expr.id->name, lhs->as.identifier_expr.id->length) == 0) {
                                // Found param, get arg
                                int a_idx = 0;
                                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                                    if (a_idx == p_idx) { arg_lhs = a->expr; break; }
                                    a_idx++;
                                }
                                break;
                            }
                            p_idx++;
                        }
                    } else {
                        arg_lhs = lhs; // Literal or other
                    }
                    
                    if (rhs->kind == EXPR_IDENTIFIER) {
                        int p_idx = 0;
                        for (DeclList *p = callee_decl->as.function_decl.params; p; p = p->next) {
                            if (p->decl->as.variable_decl.name->length == rhs->as.identifier_expr.id->length &&
                                strncmp(p->decl->as.variable_decl.name->name, rhs->as.identifier_expr.id->name, rhs->as.identifier_expr.id->length) == 0) {
                                // Found param, get arg
                                int a_idx = 0;
                                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                                    if (a_idx == p_idx) { arg_rhs = a->expr; break; }
                                    a_idx++;
                                }
                                break;
                            }
                            p_idx++;
                        }
                    } else {
                        arg_rhs = rhs; // Literal or other
                    }
                    
                    if (arg_lhs && arg_rhs) {
                        // Construct temp binary expr
                        Expr temp;
                        temp.kind = EXPR_BINARY;
                        temp.as.binary_expr.op = op;
                        temp.as.binary_expr.left = arg_lhs;
                        temp.as.binary_expr.right = arg_rhs;
                        
                        int result = sema_check_condition(&temp, sema_ranges);
                        if (result != 1) {
                             if (result == 0) {
                                 fprintf(stderr, "Error: Pre-condition violation. Arguments cannot satisfy contract.\n");
                             } else {
                                 fprintf(stderr, "Error: Pre-condition violation. Cannot prove contract is satisfied.\n");
                             }
                             exit(1);
                        }
                    }
                }
            }
        }
        
        // Verify 'in' constraints at call site
        // e.g. func get(arr int[], i int in arr) - verify i is in bounds of arr
        int param_idx = 0;
        for (DeclList *p = callee_decl->as.function_decl.params; p; p = p->next) {
            if (p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.in_field) {
                Id *arr_name = p->decl->as.variable_decl.in_field;
                
                // Find the argument for this parameter
                Expr *idx_arg = NULL;
                int a_idx = 0;
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    if (a_idx == param_idx) { idx_arg = a->expr; break; }
                    a_idx++;
                }
                
                // Find the array argument by name
                int arr_param_idx = 0;
                Type *arr_type = NULL;
                Expr *arr_arg = NULL;
                for (DeclList *arr_p = callee_decl->as.function_decl.params; arr_p; arr_p = arr_p->next) {
                    if (arr_p->decl->kind == DECL_VARIABLE) {
                        Id *an = arr_p->decl->as.variable_decl.name;
                        if (an->length == arr_name->length &&
                            strncmp(an->name, arr_name->name, an->length) == 0) {
                            arr_type = arr_p->decl->as.variable_decl.type;
                            // Get corresponding arg
                            int aa_idx = 0;
                            for (ExprList *aa = e->as.call_expr.args; aa; aa = aa->next) {
                                if (aa_idx == arr_param_idx) { arr_arg = aa->expr; break; }
                                aa_idx++;
                            }
                            break;
                        }
                    }
                    arr_param_idx++;
                }
                
                if (idx_arg && arr_arg) {
                    // Get the range of the index argument
                    Range idx_range = sema_eval_range(idx_arg, sema_ranges);
                    
                    // Get the array length (from type or expression)
                    int64_t arr_len = -1;
                    if (arr_arg->type && arr_arg->type->kind == TYPE_ARRAY && arr_arg->type->array_len >= 0) {
                        arr_len = arr_arg->type->array_len;
                    } else if (arr_type && arr_type->kind == TYPE_ARRAY && arr_type->array_len >= 0) {
                        arr_len = arr_type->array_len;
                    }
                    
                    if (arr_len > 0 && idx_range.known) {
                        // Verify: idx >= 0 and idx < arr_len
                        if (idx_range.min < 0 || idx_range.max >= arr_len) {
                            fprintf(stderr, "Error: Index out of bounds. Index range [%ld, %ld] not in [0, %ld).\n",
                                    (long)idx_range.min, (long)idx_range.max, (long)arr_len);
                            exit(1);
                        }
                    } else if (idx_range.known && idx_range.min < 0) {
                        // At least verify non-negative
                        fprintf(stderr, "Error: Index may be negative. Index range [%ld, %ld].\n",
                                (long)idx_range.min, (long)idx_range.max);
                        exit(1);
                    }
                }
            }
            param_idx++;
        }
        
        // Verify equation-style constraints at call site
        // e.g. func div(a int, b int != 0) - verify argument for b != 0
        param_idx = 0;
        for (DeclList *p = callee_decl->as.function_decl.params; p; p = p->next) {
            if (p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.constraints) {
                // Find the argument for this parameter
                Expr *arg = NULL;
                int a_idx = 0;
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    if (a_idx == param_idx) { arg = a->expr; break; }
                    a_idx++;
                }
                
                if (arg) {
                    // For each constraint on this parameter
                    for (ExprList *c = p->decl->as.variable_decl.constraints; c; c = c->next) {
                        if (c->expr->kind == EXPR_BINARY) {
                            // Substitute param with arg in the constraint
                            Expr temp;
                            temp.kind = EXPR_BINARY;
                            temp.as.binary_expr.op = c->expr->as.binary_expr.op;
                            temp.as.binary_expr.left = arg;  // Substitute param with arg
                            temp.as.binary_expr.right = c->expr->as.binary_expr.right;
                            
                            int result = sema_check_condition(&temp, sema_ranges);
                            if (result == 0) {
                                fprintf(stderr, "Error: Constraint violation. Argument does not satisfy '%s' constraint.\n",
                                        token_kind_to_str(c->expr->as.binary_expr.op));
                                exit(1);
                            } else if (result == -1) {
                                // Cannot prove - could warn or error depending on strictness
                                fprintf(stderr, "Warning: Cannot statically verify constraint '%s'.\n",
                                        token_kind_to_str(c->expr->as.binary_expr.op));
                            }
                        }
                    }
                }
            }
            param_idx++;
        }
    }
    // function-call expression type is the callee's type (return type)
    e->type = e->as.call_expr.callee->type;
    break;
  }

  case EXPR_BINARY:
    sema_infer_expr(e->as.binary_expr.left);
    sema_infer_expr(e->as.binary_expr.right);
    e->type = get_builtin_int_type();
    break;

  case EXPR_UNARY:
    sema_infer_expr(e->as.unary_expr.right);
    e->type = get_builtin_int_type();
    break;

  case EXPR_STRING: {
    // A compile-time fixed-length slice (string literal)
    size_t L = (size_t)e->as.string_expr.length;

    // element type = u8
    Type *elem = get_builtin_u8_type();

    // carve out the slice-type in the arena
    Type *slice_ty = arena_push_aligned(sema_arena, Type);
    slice_ty->kind = TYPE_SLICE;
    slice_ty->mode = MODE_SHARED;  // string literals are shared by default
    slice_ty->element_type = elem;

    // FIXED-LENGTH: no sentinel data, just record the length
    slice_ty->sentinel_str = NULL;
    slice_ty->sentinel_len = (isize)L;
    slice_ty->sentinel_is_string = false;

    e->type = slice_ty;
    break;
  }

  case EXPR_LITERAL:
    e->type = get_builtin_int_type();
    break;

  case EXPR_RANGE:
    sema_infer_expr(e->as.range_expr.start);
    sema_infer_expr(e->as.range_expr.end);
    // leave e->type NULL if never used
    break;

  case EXPR_MOVE:
    sema_infer_expr(e->as.move_expr.expr);
    e->type = type_move(sema_arena, e->as.move_expr.expr->type);
    break;

  case EXPR_MUT:
    sema_infer_expr(e->as.mut_expr.expr);
    e->type = type_mut(sema_arena, e->as.mut_expr.expr->type);
    break;

  default:
    break;
  }
}

#endif /* SEMA_TYPECHECK_H */
