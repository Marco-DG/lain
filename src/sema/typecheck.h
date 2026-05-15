#ifndef SEMA_TYPECHECK_H
#define SEMA_TYPECHECK_H

#include <limits.h>
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
extern bool sema_walk_phase;        // Defined in sema.h

// ...



// ...



/*─────────────────────────────────────────────────────────────────╗
│ 1) Helpers to get a builtin “int” Type* only once               │
╚─────────────────────────────────────────────────────────────────*/

Type *get_builtin_i32_type(void) {
  // Q-002 / int-removal: the default integer type for naked literals
  // is i32. The function name is kept for historical reasons but the
  // returned Type is concretely i32.
  static Type *int_ty = NULL;
  if (!int_ty) {
    Id *id = arena_push_aligned(sema_arena, Id);
    id->name = "i32";
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
│ 1b) Implicit integer widening helpers                          │
╚─────────────────────────────────────────────────────────────────*/

// Q-002 helpers: parse iN / uN. Returns 0 if not an iN/uN type.
// On success, *out_bits is the N (1..64), *out_signed is true for iN.
static int parse_iN_uN(Type *t, int *out_bits, bool *out_signed) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return 0;
    const char *n = t->base_type->name;
    isize len = t->base_type->length;
    if (len < 2 || len > 3) return 0;
    if (n[0] != 'i' && n[0] != 'u') return 0;
    int v = 0;
    for (isize k = 1; k < len; k++) {
        if (n[k] < '0' || n[k] > '9') return 0;
        v = v * 10 + (n[k] - '0');
    }
    if (v < 1 || v > 64) return 0;
    *out_bits = v;
    *out_signed = (n[0] == 'i');
    return 1;
}

static bool is_integer_type(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return false;
    const char *n = t->base_type->name;
    isize len = t->base_type->length;
    // Generic iN / uN (Q-002: N=1..64)
    int bits; bool sgn;
    if (parse_iN_uN(t, &bits, &sgn)) return true;
    // Pointer-sized.
    if (len == 5 && (memcmp(n,"usize",5)==0 || memcmp(n,"isize",5)==0)) return true;
    // `int` documented alias of i32.
    if (len == 3 && memcmp(n, "int", 3) == 0) return true;
    return false;
}

// Q-002 Phase 5 helpers: range of a sized integer type and fit check.
// Returns 1 if t has a known fixed-width integer range; sets *out_lo/*out_hi.
// Handles iN/uN with N ∈ [1, 64] plus legacy `int` (treated as i32).
// Non-static so ranges.h (included earlier) can forward-declare it.
int type_integer_range(Type *t, long long *out_lo, long long *out_hi) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return 0;
    int bits; bool sgn;
    if (parse_iN_uN(t, &bits, &sgn)) {
        if (sgn) {
            // i64: half-bound trick to avoid signed overflow with 1LL<<63.
            if (bits == 64) {
                *out_lo = LLONG_MIN;
                *out_hi = LLONG_MAX;
            } else {
                *out_lo = -(1LL << (bits - 1));
                *out_hi =  (1LL << (bits - 1)) - 1;
            }
        } else {
            *out_lo = 0;
            if (bits >= 63) {
                *out_hi = LLONG_MAX;  // uN with N ∈ [63, 64] approximated
            } else {
                *out_hi = (1LL << bits) - 1;
            }
        }
        return 1;
    }
    // `int` alias → i32 range.
    const char *n = t->base_type->name;
    isize len = t->base_type->length;
    if (len == 3 && memcmp(n, "int", 3) == 0) {
        *out_lo = -2147483648LL;
        *out_hi =  2147483647LL;
        return 1;
    }
    return 0;
}

// Q-002 Phase 5 (extended): check if a value's VRA range fits the
// target integer type's natural range. Returns true if check passed
// (or was skipped: unsafe block, unknown range, unbounded range).
// On violation, emits E086 with helpful suggestions and exits.
//
// `context` describes the boundary site for the error message
// (e.g., "assignment to", "return from function", "argument to").
// `target_label` identifies the specific target (variable name,
// parameter name, etc.) for the error.
bool check_value_fits_type(Range r, Type *target_type,
                           isize line, isize col,
                           const char *context, const char *target_label);
bool check_value_fits_type(Range r, Type *target_type,
                           isize line, isize col,
                           const char *context, const char *target_label) {
    if (sema_in_unsafe_block) return true;
    if (!r.known) return true;
    // Skip if range is effectively unbounded — VRA may have widened from
    // an unconstrained value, so a range whose extremum sits within a
    // small window of LLONG_MIN/LLONG_MAX is treated as "no info".
    // The window absorbs arithmetic from type-max constants
    // (e.g. `n - 2` widens to [LLONG_MIN+2, LLONG_MAX-2]).
    const long long UNBOUNDED_WINDOW = 4096;
    if (r.min <= LLONG_MIN + UNBOUNDED_WINDOW ||
        r.max >= LLONG_MAX - UNBOUNDED_WINDOW) return true;
    long long tlo, thi;
    if (!target_type || !type_integer_range(target_type, &tlo, &thi)) return true;
    if (r.min < tlo || r.max > thi) {
        const char *type_name = "?";
        int type_len = 1;
        if (target_type->kind == TYPE_SIMPLE && target_type->base_type) {
            type_name = target_type->base_type->name;
            type_len = (int)target_type->base_type->length;
        }
        fprintf(stderr,
            "[E086] Error Ln %li, Col %li: %s '%s' would overflow target type '%.*s'.\n"
            "       Value range [%lld, %lld] does not fit type range [%lld, %lld].\n"
            "       Options to resolve:\n"
            "         (a) Widen the target type (e.g., u16 instead of u8).\n"
            "         (b) Use a wrapping operator: +%%, -%%, *%% (modular).\n"
            "         (c) Use a saturating operator: +|, -|, *| (clamp).\n"
            "         (d) Tighten input constraints so VRA can prove safety.\n",
            (long)line, (long)col, context,
            target_label ? target_label : "",
            type_len, type_name,
            (long long)r.min, (long long)r.max, tlo, thi);
        diagnostic_show_line(line, col);
        exit(1);
    }
    return true;
}

// Rank in the implicit widening order (Q-002 extended).
// Rank is essentially the container bit-width category:
//   N=1..8  → 1     (8-bit container)
//   N=9..16 → 2     (16-bit container)
//   N=17..32→ 3     (32-bit container)
//   N=33..64→ 4     (64-bit container)
//   usize/isize → 5 (pointer-sized, may be 32 or 64)
//   int → 3         (default i32 monomorphization)
int integer_rank(Type *t);
int integer_rank(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return 0;
    int bits; bool sgn;
    if (parse_iN_uN(t, &bits, &sgn)) {
        if (bits <= 8) return 1;
        if (bits <= 16) return 2;
        if (bits <= 32) return 3;
        return 4;  // 33..64
    }
    const char *n = t->base_type->name;
    isize len = t->base_type->length;
    if (len == 5 && (memcmp(n,"usize",5)==0 || memcmp(n,"isize",5)==0)) return 5;
    if (len == 3 && memcmp(n, "int", 3) == 0) return 3;  // alias of i32
    return 0;
}

static Type *wider_integer_type(Type *a, Type *b) {
    return integer_rank(a) >= integer_rank(b) ? a : b;
}

// (Q-002 Phase 4 paradigm_b_result_type was reverted.
//  Rationale: `iN op iM = iK` smallest-container widening was
//  cognitively surprising (i8 + i8 = i9). The result type now
//  follows Sprint 10 widening (`iN op iM = max(iN, iM)`); overflow
//  is caught at the assignment boundary via Phase 5 (E086) when
//  the source VRA range doesn't fit the target type.)

static bool can_widen_to(Type *from, Type *to) {
    if (!is_integer_type(from) || !is_integer_type(to)) return false;
    return integer_rank(from) <= integer_rank(to);
}

/* ─────────────────────────────────────────────────────────────────╗
│ Sprint 5 (Q-004 step A): pointer-bearing type detection           │
│ A type is "pointer-bearing" if any of its values can carry a      │
│ raw pointer (and therefore borrow tracking is relevant).          │
╚─────────────────────────────────────────────────────────────────*/

static bool is_pointer_bearing(Type *t) {
    if (!t) return false;
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!t) return false;
    // Direct pointer, slice (T[]), null-terminated slice (T[:0])
    if (t->kind == TYPE_POINTER) return true;
    if (t->kind == TYPE_SLICE)   return true;
    if (t->kind == TYPE_ARRAY) {
        // Dynamic-length array (slice) is pointer-bearing.
        // Fixed-size array (length >= 0) is NOT (data is inline).
        if (t->array_len == -1) return true;
        return false;
    }
    // TYPE_SIMPLE: check the underlying decl's fields recursively.
    if (t->kind == TYPE_SIMPLE && t->base_type) {
        char buf[256];
        if ((size_t)t->base_type->length >= sizeof(buf)) return false;
        memcpy(buf, t->base_type->name, t->base_type->length);
        buf[t->base_type->length] = '\0';
        extern Symbol *sema_lookup(const char *name);
        Symbol *sym = sema_lookup(buf);
        if (!sym || !sym->decl) return false;
        if (sym->decl->kind == DECL_STRUCT) {
            for (DeclList *f = sym->decl->as.struct_decl.fields; f; f = f->next) {
                if (f->decl && f->decl->kind == DECL_VARIABLE) {
                    if (is_pointer_bearing(f->decl->as.variable_decl.type)) return true;
                }
            }
        }
        if (sym->decl->kind == DECL_ENUM) {
            for (Variant *v = sym->decl->as.enum_decl.variants; v; v = v->next) {
                for (DeclList *f = v->fields; f; f = f->next) {
                    if (f->decl && f->decl->kind == DECL_VARIABLE) {
                        if (is_pointer_bearing(f->decl->as.variable_decl.type)) return true;
                    }
                }
            }
        }
    }
    return false;
}

// F-022 support: structural type compatibility for argument-vs-field check.
// Conservative: returns true for same simple type, integer widening,
// pointer-to-same-element, or if either operand has no inferred type.
// Returns false on clear mismatches (int vs bool, struct A vs struct B, etc.).
static bool types_compatible(Type *from, Type *to) {
    if (!from || !to) return true;  // missing info → skip
    // Unwrap comptime wrappers
    while (from && from->kind == TYPE_COMPTIME) from = from->element_type;
    while (to && to->kind == TYPE_COMPTIME) to = to->element_type;
    if (!from || !to) return true;
    // Integer widening
    if (is_integer_type(from) && is_integer_type(to)) {
        return can_widen_to(from, to);
    }
    if (from->kind != to->kind) return false;
    switch (from->kind) {
        case TYPE_SIMPLE: {
            if (!from->base_type || !to->base_type) return true;
            if (from->base_type->length != to->base_type->length) return false;
            return strncmp(from->base_type->name, to->base_type->name,
                           from->base_type->length) == 0;
        }
        case TYPE_POINTER:
            return types_compatible(from->element_type, to->element_type);
        case TYPE_ARRAY:
            // Same element; length must match when both known
            if (!types_compatible(from->element_type, to->element_type)) return false;
            if (from->array_len >= 0 && to->array_len >= 0 &&
                from->array_len != to->array_len) return false;
            return true;
        case TYPE_SLICE:
            return types_compatible(from->element_type, to->element_type);
        default:
            return true; // conservative
    }
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
  return NULL;
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
    if (d) {
        if (d->kind == DECL_ENUM &&
            d->as.enum_decl.type_name->length == adt_name->length &&
            strncmp(d->as.enum_decl.type_name->name, adt_name->name, adt_name->length) == 0) {
          return &d->as.enum_decl;
        }
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
    // ONLY valid if the target resolves to the Enum declaration itself!
    if (e->as.member_expr.target->kind == EXPR_IDENTIFIER || e->as.member_expr.target->kind == EXPR_TYPE) {
        if (e->as.member_expr.target->decl && e->as.member_expr.target->decl->kind == DECL_ENUM) {
            DeclEnum *adt = &e->as.member_expr.target->decl->as.enum_decl;
            Variant *v = lookup_adt_variant(adt, e->as.member_expr.member);
            if (!v) {
                fprintf(stderr, "sema error Ln %li, Col %li: ADT has no variant '%.*s'\n",
                        e->line, e->col,
                        (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
            // ADT variant evaluates to the ADT instance type
            e->type = e->as.member_expr.target->as.type_expr.type_value;
            e->decl = e->as.member_expr.target->decl;
            return;
        }
        // If it's EXPR_IDENTIFIER but NOT an enum, it's a variable instance (e.g. `s.Circle`). Falls down.
    }

    assert(t && "member on untyped target");
    
    // Unwrap wrappers (mut, mov, ptr)
    t = sema_unwrap_type(t);

    if (t->kind == TYPE_VARIANT) {
        // We are accessing a field of an ADT variant payload (e.g. radius in shape.Circle.radius)
        Variant *v = t->variant;
        for (DeclList *f = v->fields; f; f = f->next) {
            Id *fname = f->decl->as.variable_decl.name;
            if (fname->length == e->as.member_expr.member->length &&
                strncmp(fname->name, e->as.member_expr.member->name, fname->length) == 0) {
                e->type = f->decl->as.variable_decl.type;
                return;
            }
        }
        fprintf(stderr, "sema error Ln %li, Col %li: variant '%.*s' has no field '%.*s'\n",
                e->line, e->col,
                (int)v->name->length, v->name->name,
                (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    
    // ADT Direct Unsafe Unpacking
    DeclEnum *adt_decl = NULL;
    if (t->kind == TYPE_SIMPLE && (adt_decl = find_adt_decl(t->base_type)) != NULL) {
        Variant *v = lookup_adt_variant(adt_decl, e->as.member_expr.member);
        if (v) {
            if (!sema_in_unsafe_block) {
                fprintf(stderr, "sema error Ln %li, Col %li: Direct ADT field access ('%.*s.%.*s') is only allowed inside an 'unsafe' block.\n",
                        e->line, e->col,
                        (int)t->base_type->length, t->base_type->name,
                        (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
            
            Type *var_type = arena_push_aligned(sema_arena, Type);
            var_type->kind = TYPE_VARIANT;
            var_type->variant = v;
            e->type = var_type;
            return;
        }
    }

    // If target is a slice or array, handle the common fields: .len and .data
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) {
      Id *mem = e->as.member_expr.member;
      if (mem && mem->length == 3 && strncmp(mem->name, "len", 3) == 0) {
        // .len → integer
        e->type = get_builtin_i32_type();
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
    
    if (!e->type) {
        // UFCS Fallback check: does a function exist?
        char mbuf[256];
        int mlen = e->as.member_expr.member->length < 255 ? e->as.member_expr.member->length : 255;
        memcpy(mbuf, e->as.member_expr.member->name, mlen);
        mbuf[mlen] = '\0';
        Symbol *sym = sema_lookup(mbuf);
        if (sym && sym->decl && (sym->decl->kind == DECL_FUNCTION || sym->decl->kind == DECL_PROCEDURE || sym->decl->kind == DECL_EXTERN_FUNCTION || sym->decl->kind == DECL_EXTERN_PROCEDURE)) {
            // It might be a UFCS method call (e.g., `l.consume()`).
            // We leave `e->type = NULL`. The parent `EXPR_CALL` will detect this
            // and rewrite the AST to `consume(l)`.
        } else {
            fprintf(stderr, "sema error: struct '%.*s' has no field '%.*s'\n",
                (int)t->base_type->length, t->base_type->name, 
                (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
            exit(1);
        }
    }
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
    
    // Check for UFCS: `a.method(b)` -> `method(a, b)`
    // If the callee is EXPR_MEMBER and it failed to find a field (type is NULL),
    // we assume it's a UFCS call.
    if (e->as.call_expr.callee->kind == EXPR_MEMBER && !e->as.call_expr.callee->type) {
        Expr *target = e->as.call_expr.callee->as.member_expr.target;
        Id *method_name = e->as.call_expr.callee->as.member_expr.member;
        
        char mbuf[256];
        int mlen = method_name->length < 255 ? method_name->length : 255;
        memcpy(mbuf, method_name->name, mlen);
        mbuf[mlen] = '\0';
        
        Symbol *sym = sema_lookup(mbuf);
        if (sym && sym->decl && (sym->decl->kind == DECL_FUNCTION || sym->decl->kind == DECL_PROCEDURE || sym->decl->kind == DECL_EXTERN_FUNCTION || sym->decl->kind == DECL_EXTERN_PROCEDURE)) {
            // It is a valid function! We convert the AST node to represent a UFCS call.
            // 1. Change the callee to simply be the function identifier
            Expr *new_callee = arena_push(sema_arena, Expr);
            new_callee->kind = EXPR_IDENTIFIER;
            new_callee->as.identifier_expr.id = method_name;
            new_callee->line = e->as.call_expr.callee->line;
            new_callee->col = e->as.call_expr.callee->col;
            
            // 2. Prepend the target as the first argument.
            // F-021: UFCS auto-wraps the target to match the first param's
            // ownership mode (var/mov). Auto-wrapping was previously only
            // applied for MUTABLE, leaving OWNED to fail silently. Now both
            // are handled symmetrically.
            ExprList *new_arg = arena_push(sema_arena, ExprList);
            DeclList *params = sym->decl->as.function_decl.params;
            OwnershipMode first_mode = MODE_SHARED;
            if (params && params->decl->kind == DECL_VARIABLE &&
                params->decl->as.variable_decl.type) {
                first_mode = params->decl->as.variable_decl.type->mode;
            }
            if (first_mode == MODE_MUTABLE && target->kind != EXPR_MUT) {
                Expr *mut_target = arena_push(sema_arena, Expr);
                mut_target->kind = EXPR_MUT;
                mut_target->as.mut_expr.expr = target;
                mut_target->line = target->line;
                mut_target->col = target->col;
                new_arg->expr = mut_target;
            } else if (first_mode == MODE_OWNED && target->kind != EXPR_MOVE) {
                Expr *mov_target = arena_push(sema_arena, Expr);
                mov_target->kind = EXPR_MOVE;
                mov_target->as.move_expr.expr = target;
                mov_target->line = target->line;
                mov_target->col = target->col;
                new_arg->expr = mov_target;
            } else {
                new_arg->expr = target;
            }
            
            new_arg->next = e->as.call_expr.args;
            
            // 3. Update the call expression
            e->as.call_expr.callee = new_callee;
            e->as.call_expr.args = new_arg;
            
            // Now proceed with normal call logic
            sema_infer_expr(e->as.call_expr.callee);
        } else {
            fprintf(stderr, "sema error: struct field or UFCS method '%.*s' not found on type '%.*s'\n",
                    (int)method_name->length, method_name->name,
                    (int)target->type->base_type->length, target->type->base_type->name);
            exit(1);
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
    
    // Argument count check — skip extern functions (may be variadic like printf)
    {
        Decl *cd = e->as.call_expr.callee->decl;
        if (cd && (cd->kind == DECL_FUNCTION || cd->kind == DECL_PROCEDURE)) {
            int n_params = 0, n_args = 0;
            for (DeclList *p = cd->as.function_decl.params; p; p = p->next) n_params++;
            for (ExprList *a = e->as.call_expr.args; a; a = a->next) n_args++;
            if (n_args != n_params) {
                Id *fn_name = cd->as.function_decl.name;
                fprintf(stderr, "[E012] Error Ln %li, Col %li: function '%.*s' expects %d argument(s), got %d.\n",
                        (long)e->line, (long)e->col,
                        (int)fn_name->length, fn_name->name,
                        n_params, n_args);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
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

                            // F-025: initialize line/col so any diagnostic
                            // path inside sema_check_condition reports a
                            // location tied to the offending argument.
                            Expr temp = {0};
                            temp.kind = EXPR_BINARY;
                            temp.as.binary_expr.op = c->expr->as.binary_expr.op;
                            temp.as.binary_expr.left = lhs_arg;
                            temp.as.binary_expr.right = rhs_arg;
                            temp.line = lhs_arg ? lhs_arg->line : e->line;
                            temp.col  = lhs_arg ? lhs_arg->col  : e->col;

                            int result = sema_check_condition(&temp, sema_ranges);
                            if (result == 0) {
                                fprintf(stderr, "[E012] Error Ln %li, Col %li: Constraint violation. Argument does not satisfy '%s' constraint.\n",
                                        (long)temp.line, (long)temp.col,
                                        token_kind_to_str(c->expr->as.binary_expr.op));
                                diagnostic_show_line(temp.line, temp.col);
                                exit(1);
                            } else if (result == -1) {
                                // check safety policy
                            }
                        }
                    }
                }
            }

            // Q-002 Phase 5: overflow-at-boundary check (call argument).
            // Skip integer literals (polymorphic across iN/uN — trusted to fit).
            // Only fire during walk phase: during resolve, VRA hasn't yet
            // built up local variable ranges so we'd hit false positives.
            if (sema_walk_phase && p->decl->kind == DECL_VARIABLE && sema_ranges) {
                Type *ptype = p->decl->as.variable_decl.type;
                Expr *parg = NULL;
                int a_idx = 0;
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    if (a_idx == param_idx) { parg = a->expr; break; }
                    a_idx++;
                }
                if (parg && ptype && parg->kind != EXPR_LITERAL) {
                    Range r = sema_eval_range(parg, sema_ranges);
                    Id *pname = p->decl->as.variable_decl.name;
                    char buf[160];
                    int n = pname ? (int)pname->length : 0;
                    if (n > 159) n = 159;
                    if (n) memcpy(buf, pname->name, n);
                    buf[n] = '\0';
                    check_value_fits_type(r, ptype, parg->line, parg->col,
                        "argument to parameter", buf);
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
            // F-022 fix: verify argument type matches field type.
            if (f->decl && f->decl->kind == DECL_VARIABLE && a->expr) {
                Type *field_ty = f->decl->as.variable_decl.type;
                Type *arg_ty = a->expr->type;
                // Integer literals are polymorphic across integer types:
                // skip the widen check when the arg is a literal assigned to
                // an integer field (the literal value is trusted to fit).
                bool literal_to_int =
                    a->expr->kind == EXPR_LITERAL &&
                    field_ty && is_integer_type(field_ty);
                if (!literal_to_int && field_ty && arg_ty &&
                    !types_compatible(arg_ty, field_ty)) {
                    Id *fname = f->decl->as.variable_decl.name;
                    fprintf(stderr,
                            "[E012] Error Ln %li, Col %li: struct '%.*s' field '%.*s' type mismatch at argument %d.\n",
                            (long)e->line, (long)e->col,
                            (int)callee_decl->as.struct_decl.name->length,
                            callee_decl->as.struct_decl.name->name,
                            (int)fname->length, fname->name,
                            field_count + 1);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
                // Q-002 Phase 5: overflow-at-boundary (struct field init).
                if (sema_walk_phase && sema_ranges && field_ty
                    && a->expr->kind != EXPR_LITERAL) {
                    Range r = sema_eval_range(a->expr, sema_ranges);
                    Id *fname = f->decl->as.variable_decl.name;
                    char buf[160];
                    int n = fname ? (int)fname->length : 0;
                    if (n > 159) n = 159;
                    if (n) memcpy(buf, fname->name, n);
                    buf[n] = '\0';
                    check_value_fits_type(r, field_ty, a->expr->line, a->expr->col,
                        "struct field initialization", buf);
                }
            }
            f = f->next;
            a = a->next;
            field_count++;
            arg_count++;
        }
        
        while (f) { field_count++; f = f->next; }
        while (a) { arg_count++; a = a->next; }
        
        if (arg_count < field_count) {
            fprintf(stderr, "[E012] Error Ln %li, Col %li: Partial initialization of struct '%.*s'. Expected %d arguments, got %d.\n",
                    e->line, e->col,
                    (int)callee_decl->as.struct_decl.name->length, callee_decl->as.struct_decl.name->name,
                    field_count, arg_count);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        } else if (arg_count > field_count) {
            fprintf(stderr, "[E012] Error Ln %li, Col %li: Too many arguments for struct '%.*s'. Expected %d, got %d.\n",
                    e->line, e->col,
                    (int)callee_decl->as.struct_decl.name->length, callee_decl->as.struct_decl.name->name,
                    field_count, arg_count);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    // function-call expression type is the callee's type (return type)
    if (callee_decl && callee_decl->kind == DECL_STRUCT) {
        e->type = e->as.call_expr.callee->as.type_expr.type_value;
    } else if (callee_decl && callee_decl->kind == DECL_ENUM) {
        if (e->as.call_expr.callee->kind == EXPR_MEMBER) {
            e->type = e->as.call_expr.callee->as.member_expr.target->as.type_expr.type_value;
        } else {
            e->type = e->as.call_expr.callee->type;
        }
    } else {
        e->type = e->as.call_expr.callee->type;
    }
    break;
  }

  case EXPR_BINARY:
    sema_infer_expr(e->as.binary_expr.left);

    // For 'and' chains: if LHS is/contains 'in', push in-guard before evaluating RHS
    // This allows: while l.pos in l.src and l.src[l.pos] != '"'
    if (e->as.binary_expr.op == TOKEN_KEYWORD_AND) {
        InGuardEntry *old_guards = sema_in_guards;
        sema_push_in_guards(e->as.binary_expr.left);
        sema_infer_expr(e->as.binary_expr.right);
        sema_in_guards = old_guards;
    } else {
        sema_infer_expr(e->as.binary_expr.right);
    }

    // 'in' operator: result is bool, skip other checks
    if (e->as.binary_expr.op == TOKEN_KEYWORD_IN) {
        e->type = get_builtin_i32_type();
        break;
    }

    // Division/modulo by zero check in pure func (must be total).
    // Parameter constraints (e.g., `b int != 0`) guarantee safety.
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        TokenKind op = e->as.binary_expr.op;
        if (op == TOKEN_SLASH || op == TOKEN_PERCENT) {
            Range rhs_range = sema_eval_range(e->as.binary_expr.right, sema_ranges);
            
            // Check if divisor is provably non-zero
            bool proven_nonzero = false;
            
            // Case 1: VRA proves range excludes zero
            if (rhs_range.known && (rhs_range.min > 0 || rhs_range.max < 0)) {
                proven_nonzero = true;
            }
            
            // Case 2: Divisor is a parameter with `!= 0` constraint
            if (!proven_nonzero && e->as.binary_expr.right->kind == EXPR_IDENTIFIER) {
                Decl *rhs_decl = e->as.binary_expr.right->decl;
                if (rhs_decl && rhs_decl->kind == DECL_VARIABLE && rhs_decl->as.variable_decl.constraints) {
                    for (ExprList *c = rhs_decl->as.variable_decl.constraints; c; c = c->next) {
                        if (c->expr->kind == EXPR_BINARY && 
                            c->expr->as.binary_expr.op == TOKEN_BANG_EQUAL &&
                            c->expr->as.binary_expr.right->kind == EXPR_LITERAL &&
                            c->expr->as.binary_expr.right->as.literal_expr.value == 0) {
                            proven_nonzero = true;
                            break;
                        }
                    }
                }
            }
            
            if (!proven_nonzero) {
                if (!rhs_range.known) {
                    fprintf(stderr, "[E015] Error Ln %li, Col %li: potential division by zero in pure function '%.*s'. "
                            "The divisor's range is unknown. Use a constraint (e.g., `b int != 0`) to prove safety.\n",
                            (long)e->line, (long)e->col,
                            (int)current_function_decl->as.function_decl.name->length,
                            current_function_decl->as.function_decl.name->name);
                } else {
                    fprintf(stderr, "[E015] Error Ln %li, Col %li: potential division by zero in pure function '%.*s'. "
                            "Divisor range [%ld, %ld] includes zero. Use a constraint (e.g., `b int != 0`) to prove safety.\n",
                            (long)e->line, (long)e->col,
                            (int)current_function_decl->as.function_decl.name->length,
                            current_function_decl->as.function_decl.name->name,
                            (long)rhs_range.min, (long)rhs_range.max);
                }
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
    }

    // Struct equality check: == and != on struct/enum types is a compile error (§8.8)
    {
        TokenKind op = e->as.binary_expr.op;
        if (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL) {
            Type *lt = e->as.binary_expr.left->type;
            // Check if either side is a struct type
            if (lt && lt->kind == TYPE_SIMPLE && lt->base_type) {
                char lbuf[256];
                int ll = lt->base_type->length < 255 ? lt->base_type->length : 255;
                memcpy(lbuf, lt->base_type->name, ll);
                lbuf[ll] = '\0';
                Symbol *lsym = sema_lookup(lbuf);
                if (lsym && lsym->decl && (lsym->decl->kind == DECL_STRUCT || lsym->decl->kind == DECL_ENUM)) {
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: cannot use '%s' on struct/enum type '%s'. "
                            "Implement an 'equals' method and use it instead.\n",
                            (long)e->line, (long)e->col,
                            op == TOKEN_EQUAL_EQUAL ? "==" : "!=", lbuf);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
        }
    }

    {
        TokenKind bop = e->as.binary_expr.op;
        if (bop == TOKEN_EQUAL_EQUAL || bop == TOKEN_BANG_EQUAL ||
            bop == TOKEN_ANGLE_BRACKET_LEFT || bop == TOKEN_ANGLE_BRACKET_LEFT_EQUAL ||
            bop == TOKEN_ANGLE_BRACKET_RIGHT || bop == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL ||
            bop == TOKEN_KEYWORD_AND || bop == TOKEN_KEYWORD_OR) {
            e->type = get_builtin_i32_type();
        } else {
            Type *lt = e->as.binary_expr.left->type;
            Type *rt = e->as.binary_expr.right->type;
            // Q-002 simplified (post Phase 4 rollback): result type follows
            // Sprint 10 widening — max(rank(lt), rank(rt)). Wrap/sat ops
            // keep the LHS type unchanged (the operation bounds the result
            // to that type). Overflow on plain ops is caught at the
            // assignment boundary by Phase 5 (E086).
            TokenKind aop = e->as.binary_expr.op;
            bool is_wrap_or_sat = (aop == TOKEN_PLUS_PERCENT  || aop == TOKEN_MINUS_PERCENT
                                || aop == TOKEN_ASTERISK_PERCENT || aop == TOKEN_PLUS_PIPE
                                || aop == TOKEN_MINUS_PIPE || aop == TOKEN_ASTERISK_PIPE);
            if (is_wrap_or_sat && lt && is_integer_type(lt)) {
                e->type = lt;
            } else if (lt && rt && is_integer_type(lt) && is_integer_type(rt)) {
                e->type = wider_integer_type(lt, rt);
            } else {
                e->type = (lt && is_integer_type(lt)) ? lt :
                          (rt && is_integer_type(rt)) ? rt :
                          get_builtin_i32_type();
            }
        }
    }
    break;

  case EXPR_UNARY:
    sema_infer_expr(e->as.unary_expr.right);
    if (e->as.unary_expr.op == TOKEN_ASTERISK) {
        // Dereference
        if (!e->as.unary_expr.right || !e->as.unary_expr.right->type) {
             // If operands are broken, we can't check
             // sema_resolve should have caught typical errors, but to be safe:
             // e->type = get_builtin_i32_type(); // fallback
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
             e->type = get_builtin_i32_type();
        }
    } else {
        e->type = get_builtin_i32_type();
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

  case EXPR_ARRAY_LITERAL: {
    ExprList *elems = e->as.array_literal_expr.elements;
    if (!elems) {
        fprintf(stderr, "sema error Ln %li, Col %li: empty array literal\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    Type *elem_type = NULL;
    isize count = 0;
    for (ExprList *el = elems; el; el = el->next) {
        sema_infer_expr(el->expr);
        if (!elem_type) {
            elem_type = el->expr->type;
        }
        count++;
    }
    e->type = type_array(sema_arena, elem_type, count);
    break;
  }

  case EXPR_LITERAL:
    e->type = get_builtin_i32_type();
    break;

  case EXPR_CHAR:
    e->type = get_builtin_u8_type();
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
            // Q-003.B: semi-open sub-slicing semantics. When both
            // endpoints are compile-time literals, verify start <= end.
            // VRA-tracked ranges are checked via sema_check_bounds for
            // each endpoint separately.
            Expr *rs = e->as.index_expr.index->as.range_expr.start;
            Expr *re = e->as.index_expr.index->as.range_expr.end;
            if (!sema_in_unsafe_block &&
                rs && re &&
                rs->kind == EXPR_LITERAL && re->kind == EXPR_LITERAL) {
                long long sv = (long long)rs->as.literal_expr.value;
                long long ev = (long long)re->as.literal_expr.value;
                if (sv > ev) {
                    fprintf(stderr,
                        "[E087] Error Ln %li, Col %li: sub-slice start (%lld) is "
                        "greater than end (%lld). Range must satisfy start <= end.\n",
                        (long)e->line, (long)e->col, sv, ev);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
        } else {
            e->type = t->element_type;
        }
        // STATIC BOUNDS CHECK (only during walk phase, skipped in unsafe/in-guarded)
        if (sema_ranges && sema_walk_phase && !sema_in_unsafe_block) {
            bool guarded = sema_is_in_guarded(e->as.index_expr.index, e->as.index_expr.target);
            if (guarded) {
                /* bounds proven by 'in' guard — skip check */
            } else {
                sema_check_bounds(sema_ranges, e->as.index_expr.index, t);
            }
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


  case EXPR_MATCH: {
    sema_infer_expr(e->as.match_expr.value);
    Type *inferred_type = NULL;
    for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
        sema_push_scope();
        for (ExprList *p = c->patterns; p; p = p->next) {
            sema_infer_expr(p->expr);
        }
        sema_infer_expr(c->body);
        sema_pop_scope();
        
        if (!inferred_type && c->body->type) {
            inferred_type = c->body->type;
        }
    }
    
    if (!sema_check_expr_match_exhaustive(e)) {
        fprintf(stderr, "[E014] Error Ln %li, Col %li: non-exhaustive match expression\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    
    // If inferred type is a fixed-length string literal type (TYPE_SLICE with
    // sentinel_str==NULL, sentinel_len>0), unify to u8[:0] so case expressions
    // with string arms of different lengths share a common result type.
    if (inferred_type && inferred_type->kind == TYPE_SLICE &&
        !inferred_type->sentinel_is_string &&
        inferred_type->sentinel_str == NULL &&
        inferred_type->sentinel_len > 0) {
        Type *slice_ty = arena_push_aligned(sema_arena, Type);
        slice_ty->kind       = TYPE_SLICE;
        slice_ty->mode       = MODE_SHARED;
        slice_ty->element_type = inferred_type->element_type;
        slice_ty->sentinel_str       = "0";
        slice_ty->sentinel_len       = 1;
        slice_ty->sentinel_is_string = false;
        inferred_type = slice_ty;
    }

    e->type = inferred_type ? inferred_type : get_builtin_i32_type();
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

  case EXPR_CAST: {
    sema_infer_expr(e->as.cast_expr.expr);
    // F-028: basic cast validity — pointer-related casts require `unsafe`.
    Type *src = e->as.cast_expr.expr ? e->as.cast_expr.expr->type : NULL;
    Type *tgt = e->as.cast_expr.target_type;
    Type *src_u = src, *tgt_u = tgt;
    while (src_u && src_u->kind == TYPE_COMPTIME) src_u = src_u->element_type;
    while (tgt_u && tgt_u->kind == TYPE_COMPTIME) tgt_u = tgt_u->element_type;
    bool src_is_ptr = src_u && src_u->kind == TYPE_POINTER;
    bool tgt_is_ptr = tgt_u && tgt_u->kind == TYPE_POINTER;
    if ((src_is_ptr || tgt_is_ptr) && !sema_in_unsafe_block) {
        fprintf(stderr, "[E012] Error Ln %li, Col %li: cast involving a raw pointer requires an 'unsafe' block.\n",
                (long)e->line, (long)e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    // type already set at parse time (target_type)
    break;
  }

  default:
    break;
  }
}

#endif /* SEMA_TYPECHECK_H */
