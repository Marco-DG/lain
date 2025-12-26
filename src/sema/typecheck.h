#ifndef SEMA_TYPECHECK_H
#define SEMA_TYPECHECK_H

#include "../ast.h"
#include "resolve.h"
#include <assert.h>
#include <string.h>

/* Forward decls used elsewhere */
Type *get_builtin_int_type(void);
Type *get_builtin_u8_type(void);
void sema_infer_expr(Expr *e);
void sema_resolve_expr(Expr *e); // forward

extern Arena *sema_arena;
extern DeclList *sema_decls;
extern Type *current_return_type;

/*
    helpers
*/

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

/* Unwrap wrappers to get the underlying type (struct/array/slice) */
static Type *sema_unwrap_type(Type *t) {
    while (t) {
        if (t->kind == TYPE_MUT) t = t->mut.base;
        else if (t->kind == TYPE_MOVE) t = t->move.base;
        else if (t->kind == TYPE_POINTER) t = t->element_type;
        else if (t->kind == TYPE_COMPTIME) t = t->element_type;
        else break;
    }
    return t;
}

void sema_infer_expr(Expr *e) {
  if (!e)
    return;
  switch (e->kind) {
  case EXPR_IDENTIFIER:
    // already set in resolve
    break;

  case EXPR_MEMBER: {
    sema_infer_expr(e->as.member_expr.target);
    Type *t = e->as.member_expr.target->type;
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

  case EXPR_INDEX: {
    // First infer the target and the index expression
    sema_infer_expr(e->as.index_expr.target);
    sema_infer_expr(e->as.index_expr.index);

    // Now check that the target really is an array or slice:
    Type *t = e->as.index_expr.target->type;
    if (!t || (t->kind != TYPE_ARRAY && t->kind != TYPE_SLICE)) {
      fprintf(stderr,
              "sema error: cannot index expression of non‐array/slice type\n");
      exit(1);
    }

    // If the index is a `..` or `..=` range, produce a new length‐based slice.
    if (e->as.index_expr.index->kind == EXPR_RANGE) {
      // Build a TYPE_ARRAY over the element_type so that we get a .len field
      // new:
        e->type = type_array(sema_arena, t->element_type, /*array_len=*/ -1);

    } else {
      // Plain indexing: element := array[i] or slice[i]
      e->type = t->element_type;
    }
    break;
  }

  case EXPR_CALL: {
    // ensure callee resolved & infer args
    sema_resolve_expr(e->as.call_expr.callee);
    for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
      sema_infer_expr(a->expr);
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
    // A compile‐time fixed‐length slice (string literal)
    size_t L = (size_t)e->as.string_expr.length;

    // element type = u8
    Type *elem = get_builtin_u8_type();

    // carve out the slice‐type in the arena
    Type *slice_ty = arena_push_aligned(sema_arena, Type);
    slice_ty->kind = TYPE_SLICE;
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
