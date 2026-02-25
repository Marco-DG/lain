#ifndef SEMA_TYPECHECK_H
#define SEMA_TYPECHECK_H

#include "../ast.h"
#include "resolve.h"
#include "resolve.h"
#include "ranges.h" // Range analysis
#include "bounds.h"  // Static bounds checking

extern Arena *sema_arena;
extern DeclList *sema_decls;
extern Type *current_return_type;
extern Decl *current_function_decl; // Defined in sema.h
extern RangeTable *sema_ranges;     // Defined in sema.h
extern bool sema_in_unsafe_block;   // Defined in sema.h

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
// (removed debug print)
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
    
    // Verify equation-style constraints at call site
    Decl *callee_decl = e->as.call_expr.callee->decl;
    if (callee_decl && (callee_decl->kind == DECL_FUNCTION || 
                        callee_decl->kind == DECL_PROCEDURE ||
                        callee_decl->kind == DECL_EXTERN_FUNCTION || 
                        callee_decl->kind == DECL_EXTERN_PROCEDURE)) {
        
        int param_idx = 0;
        DeclList *params = callee_decl->as.function_decl.params;
        
        for (DeclList *p = params; p; p = p->next) {
            
            // Check 'in' field constraint
            if (p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.in_field) {
                Id *arr_name = p->decl->as.variable_decl.in_field;
                
                // Find the argument for this parameter (index)
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
                for (DeclList *arr_p = params; arr_p; arr_p = arr_p->next) {
                    if (arr_p->decl->kind == DECL_VARIABLE) {
                        Id *an = arr_p->decl->as.variable_decl.name;
                        if (an->length == arr_name->length &&
                            strncmp(an->name, arr_name->name, an->length) == 0) {
                            arr_type = arr_p->decl->as.variable_decl.type;
                            // Get corresponding arg for array
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
                        fprintf(stderr, "Error: Index may be negative. Index range [%ld, %ld].\n",
                                (long)idx_range.min, (long)idx_range.max);
                        exit(1);
                    }
                }
            }


            if (p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.constraints) {
                // ... (inner logic) ...
                // Find the argument for this parameter (LHS of constraint)
                Expr *lhs_arg = NULL;
                int a_idx = 0;
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    if (a_idx == param_idx) { lhs_arg = a->expr; break; }
                    a_idx++;
                }

                if (lhs_arg) {
                    for (ExprList *c = p->decl->as.variable_decl.constraints; c; c = c->next) {
                        if (c->expr->kind == EXPR_BINARY) {
                            Expr *rhs_expr = c->expr->as.binary_expr.right;
                            Expr *rhs_arg = NULL;

                            if (rhs_expr->kind == EXPR_IDENTIFIER) {
                                int rhs_p_idx = 0;
                                for (DeclList *rp = params; rp; rp = rp->next) {
                                    Id *rp_name = rp->decl->as.variable_decl.name;
                                    if (rp_name->length == rhs_expr->as.identifier_expr.id->length && 
                                        strncmp(rp_name->name, rhs_expr->as.identifier_expr.id->name, rp_name->length) == 0) {
                                        int ra_idx = 0;
                                        for (ExprList *ra = e->as.call_expr.args; ra; ra = ra->next) {
                                            if (ra_idx == rhs_p_idx) { rhs_arg = ra->expr; break; }
                                            ra_idx++;
                                        }
                                        break;
                                    }
                                    rhs_p_idx++;
                                }
                                if (!rhs_arg) rhs_arg = rhs_expr; 
                            } else {
                                rhs_arg = rhs_expr; 
                            }

                            Expr temp;
                            temp.kind = EXPR_BINARY;
                            temp.as.binary_expr.op = c->expr->as.binary_expr.op;
                            temp.as.binary_expr.left = lhs_arg;
                            temp.as.binary_expr.right = rhs_arg;

                            int result = sema_check_condition(&temp, sema_ranges);
                            if (result == 0) {
                                fprintf(stderr, "Error: Constraint violation. Argument does not satisfy '%s' constraint.\n",
                                        token_kind_to_str(c->expr->as.binary_expr.op));
                                exit(1);
                            } else if (result == -1) {
                                // check safety policy
                            }
                        }
                    }
                }
            }
            param_idx++;
        }
    } else if (callee_decl && callee_decl->kind == DECL_STRUCT) {
        // Validate struct constructor arguments
        DeclList *fields = callee_decl->as.struct_decl.fields;
        ExprList *args = e->as.call_expr.args;
        int field_count = 0;
        int arg_count = 0;
        
        DeclList *f = fields;
        ExprList *a = args;
        
        while (f && a) {
            // TODO: Type compatibility check between a->expr->type and f->decl->as.variable_decl.type
            f = f->next;
            a = a->next;
            field_count++;
            arg_count++;
        }
        
        while (f) { field_count++; f = f->next; }
        while (a) { arg_count++; a = a->next; }
        
        if (arg_count < field_count) {
            fprintf(stderr, "Error Ln %li, Col %li: Partial initialization of struct '%.*s'. Expected %d arguments, got %d.\n",
                    e->line, e->col,
                    (int)callee_decl->as.struct_decl.name->length, callee_decl->as.struct_decl.name->name,
                    field_count, arg_count);
            exit(1);
        } else if (arg_count > field_count) {
            fprintf(stderr, "Error Ln %li, Col %li: Too many arguments for struct '%.*s'. Expected %d, got %d.\n",
                    e->line, e->col,
                    (int)callee_decl->as.struct_decl.name->length, callee_decl->as.struct_decl.name->name,
                    field_count, arg_count);
            exit(1);
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
    if (e->as.unary_expr.op == TOKEN_ASTERISK) {
        // Dereference
        if (!e->as.unary_expr.right || !e->as.unary_expr.right->type) {
             // If operands are broken, we can't check
             // sema_resolve should have caught typical errors, but to be safe:
             // e->type = get_builtin_int_type(); // fallback
             // return;
             // Actually let's exit to be consistent with previous panic
             fprintf(stderr, "sema error: internal: deref operand untyped\n");
             exit(1);
        }
        
        Type *t = e->as.unary_expr.right->type;
        while (t && t->kind == TYPE_COMPTIME) {
            t = t->element_type;
        }
        
        if (t->kind == TYPE_POINTER) {
            e->type = t->element_type;
            if (!sema_in_unsafe_block) {
                fprintf(stderr, "sema error: Dereference of raw pointer outside 'unsafe' block.\n");
                exit(1);
            }
        } else {
             // Non-pointer deref??
             // Likely a parsing error or a reference deref if supported.
             // For now, assume it results in element type if we can determine it, 
             // or just int if unknown.
             e->type = get_builtin_int_type();
        }
    } else {
        e->type = get_builtin_int_type();
    }
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

  case EXPR_FLOAT_LITERAL: {
    // Float literals infer to f64
    static Type *f64_ty = NULL;
    if (!f64_ty) {
      Id *id = arena_push_aligned(sema_arena, Id);
      id->name = "f64";
      id->length = 3;
      f64_ty = type_simple(sema_arena, id);
    }
    e->type = f64_ty;
    break;
  }

  case EXPR_RANGE:
    sema_infer_expr(e->as.range_expr.start);
    sema_infer_expr(e->as.range_expr.end);
    // leave e->type NULL if never used
    break;

  case EXPR_INDEX: {
    sema_infer_expr(e->as.index_expr.target);
    sema_infer_expr(e->as.index_expr.index);

    Type *t = sema_unwrap_type(e->as.index_expr.target->type);
    if (!t) {
         // Error or just return?
         break;
    }

    if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) {
        if (e->as.index_expr.index->kind == EXPR_RANGE) {
            e->type = type_array(sema_arena, t->element_type, -1);
        } else {
            e->type = t->element_type;
        }
        // STATIC BOUNDS CHECK
        if (sema_ranges) {
            sema_check_bounds(sema_ranges, e->as.index_expr.index, t);
        }
    } else if (t->kind == TYPE_POINTER) {
        // Pointer indexing syntax `ptr[i]` (unsafe or restricted?)
        // Lain spec says pointers are unsafe. But let's allow indexing if it mimics C.
        // But we have no bounds info.
        e->type = t->element_type;
    } else {
        fprintf(stderr, "sema error: indexing non-array/slice type\n");
        // exit(1); // Optional: be strict
    }
    break;
  }


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
