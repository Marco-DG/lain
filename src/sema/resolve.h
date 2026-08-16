// resolve.h : name-resolution logic

#ifndef SEMA_RESOLVE_H
#define SEMA_RESOLVE_H


#include "../ast.h"
#include "../ast_clone.h"
#include "comptime.h" // CTFE engine
#include "exhaustiveness.h"  // Match exhaustiveness checking
#include "ranges.h"          // Range analysis

/*──────────────────────────────────────────────────────────────────╗
│ TARGET PLATFORM CONSTANTS                                         │
│ Resolved at compiler-build-time via host platform detection.      │
│ In a cross-compiler these would come from command-line flags.     │
╚──────────────────────────────────────────────────────────────────*/

// OS constants for @os builtin — matches .Linux/.Windows enum values
#define LAIN_OS_LINUX   1
#define LAIN_OS_WINDOWS 2
#define LAIN_OS_MACOS   3

// Arch constants for @arch builtin
#define LAIN_ARCH_X86_64  1
#define LAIN_ARCH_AARCH64 2

#if defined(__linux__)
  #define LAIN_TARGET_OS   LAIN_OS_LINUX
#elif defined(_WIN32)
  #define LAIN_TARGET_OS   LAIN_OS_WINDOWS
#elif defined(__APPLE__)
  #define LAIN_TARGET_OS   LAIN_OS_MACOS
#else
  #define LAIN_TARGET_OS   0
#endif

#if defined(__x86_64__) || defined(_M_X64)
  #define LAIN_TARGET_ARCH LAIN_ARCH_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define LAIN_TARGET_ARCH LAIN_ARCH_AARCH64
#else
  #define LAIN_TARGET_ARCH 0
#endif


extern Type *current_return_type;
extern Decl *current_function_decl; // New
extern const char *current_module_path;

// (Q-002 Phase 3 helper removed; int monomorphization rolled back.)
extern DeclList *sema_decls;
extern Arena *sema_arena;
extern RangeTable *sema_ranges;
extern bool sema_in_unsafe_block;

Type *get_builtin_i32_type(void);
Type *get_builtin_u8_type(void);
void sema_infer_expr(Expr *e);
// Defined in typecheck.h (included after this file); checks a fn-ptr initialiser.
static void fnptr_assign_check(Type *target, Expr *rhs, isize line, isize col);
// Defined in monomorph.h; rewrites a generic call to its concrete instance.
static bool sema_monomorphize_call(Expr *call);
// Defined in monomorph.h; resolves generic type-applications `Vec(i32)` in a type.
static Type *mono_resolve_type_apps(Type *t);
static void  mono_resolve_signature(Decl *d);
void sema_resolve_expr(Expr *e); // forward

/*
    helpers
*/

// ─────────────────────────────────────────────────────────────────
// Track which enum (if any) we’re returning from right now.
// Used to resolve unqualified variant names in match arms.
// ─────────────────────────────────────────────────────────────────


// Build “module.path.field” as a single C string
void sema_build_path(Expr *e, char *buf, size_t cap) {
  if (!buf || cap == 0)
    return;
  buf[0] = '\0';

  if (e->kind == EXPR_IDENTIFIER) {
    Id *id = e->as.identifier_expr.id;
    size_t len = (size_t)id->length;
    size_t to_copy = len < (cap - 1) ? len : (cap - 1);
    memcpy(buf, id->name, to_copy);
    buf[to_copy] = '\0';
  } else if (e->kind == EXPR_MEMBER) {
    ExprMember *m = &e->as.member_expr;
    sema_build_path(m->target, buf, cap);

    size_t cur = strlen(buf);
    if (cur + 1 < cap) {
      buf[cur++] = '.';
      buf[cur] = '\0';
    }

    Id *field = m->member;
    size_t flen = (size_t)field->length;
    size_t room = cap - cur - 1;
    size_t to_copy = flen < room ? flen : room;
    memcpy(buf + cur, field->name, to_copy);
    buf[cur + to_copy] = '\0';
  } else {
    fprintf(stderr,
            "sema error: `use` target must be identifier or member-path\n");
    exit(1);
  }
}

/*─────────────────────────────────────────────────────────────────╗
│ Build‑scope: register every top‑level Decl + types           │
╚─────────────────────────────────────────────────────────────────*/
void sema_build_scope(DeclList *decls, const char *module_path) {
    // ––––––– Instead of “sema_clear_table()”, use:
    sema_clear_globals();
  
    sema_decls = decls; // for struct lookups later
  
    // Sanitize module path for C names
    char *safe_module_path = strdup(module_path);
    for (char *p = safe_module_path; *p; p++) {
        char c = *p;
        // Make it a valid C identifier: dots and non-identifier chars become '_'
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            *p = '_';
        }
    }
    // C identifiers cannot start with a digit: prepend '_'
    if (safe_module_path[0] >= '0' && safe_module_path[0] <= '9') {
        size_t old_len = strlen(safe_module_path);
        char *prefixed = malloc(old_len + 2);
        prefixed[0] = '_';
        memcpy(prefixed + 1, safe_module_path, old_len + 1);
        free(safe_module_path);
        safe_module_path = prefixed;
    }

    for (DeclList *dl = decls; dl; dl = dl->next) {
      Decl *d = dl->decl;
      if (!d)
        continue;
      
      switch (d->kind) {
      case DECL_VARIABLE: {
        // top‑level variable → insert into sema_globals
        Id *id = d->as.variable_decl.name;
        Type *typ = d->as.variable_decl.type;
  
        // raw name
        char *raw = malloc(id->length + 1);
        memcpy(raw, id->name, id->length);
        raw[id->length] = '\0';
  
        // build c_name = "<module>_<raw>"
        size_t clen = strlen(safe_module_path) + 1 /* '_' */ + id->length + 1;
        char *cname = malloc(clen);
        snprintf(cname, clen, "%s_%s", safe_module_path, raw);
        for (char *p = cname; *p; p++) if (*p == '.') *p = '_';
  
        sema_insert_global(raw, cname, typ, d, d->as.variable_decl.is_mutable);
        free(raw);
        free(cname);
        break;
      }
  
      case DECL_EXTERN_FUNCTION:
      case DECL_EXTERN_PROCEDURE:
      case DECL_FUNCTION:
      case DECL_PROCEDURE: {
        // function name + return type → insert into sema_globals
        Id *id = d->as.function_decl.name;
        Type *rt = d->as.function_decl.return_type;

        // Phase 3: Purity & Test Refactoring
        // Reject `func main` and enforce `proc main`
        if (d->kind == DECL_FUNCTION && id->length == 4 && strncmp(id->name, "main", 4) == 0) {
            fprintf(stderr, "[E013] Error Ln %li, Col %li: 'main' must be a procedure ('proc'), not a pure function ('func').\n", d->line, d->col);
            diagnostic_show_line(d->line, d->col);
            exit(1);
        }
  
        char *rawf = malloc(id->length + 1);
        memcpy(rawf, id->name, id->length);
        rawf[id->length] = '\0';
  
        char *cnamef;
        if (d->kind == DECL_EXTERN_FUNCTION || d->kind == DECL_EXTERN_PROCEDURE) {
            // Extern functions use their raw name
            cnamef = strdup(rawf);
        } else {
            size_t fclen = strlen(safe_module_path) + 1 + id->length + 1;
            cnamef = malloc(fclen);
            snprintf(cnamef, fclen, "%s_%s", safe_module_path, rawf);
            for (char *p = cnamef; *p; p++) if (*p == '.') *p = '_';
        }
  
        sema_insert_global(rawf, cnamef, rt, d, false);
        free(rawf);
        free(cnamef);
  
        // ⚠ Do ​not​ insert the parameters into sema_globals here anymore.
        //    (They will be handled later in sema_resolve_module.)
        break;
      }
  
      case DECL_STRUCT: {
        // struct type → insert into sema_globals
        Id *id = d->as.struct_decl.name;
        char *raws = malloc(id->length + 1);
        memcpy(raws, id->name, id->length);
        raws[id->length] = '\0';
  
        size_t sclen = strlen(safe_module_path) + 1 + id->length + 1;
        char *cnames = malloc(sclen);
        snprintf(cnames, sclen, "%s_%s", safe_module_path, raws);
        for (char *p = cnames; *p; p++) if (*p == '.') *p = '_';
  
        Type *sty = type_simple(sema_arena, id);
        sema_insert_global(raws, cnames, sty, d, false);
  
        free(raws);
        free(cnames);
        break;
      }

      case DECL_EXTERN_TYPE: {
        Id *id = d->as.extern_type_decl.name;
        char *raw = malloc(id->length + 1);
        memcpy(raw, id->name, id->length);
        raw[id->length] = '\0';
        char *cname = strdup(raw);
        Type *t = type_simple(sema_arena, id);
        sema_insert_global(raw, cname, t, d, false);
        free(raw);
        free(cname);
        break;
      }

      case DECL_ENUM: {
        // 1) Register the enum *type* itself (raw → c_name → Type*)
        Id *tid = d->as.enum_decl.type_name;
  
        // raw name, e.g. "Kind"
        char rawt[256];
        int lt = tid->length < (int)sizeof(rawt) - 1 ? tid->length
                                                     : (int)sizeof(rawt) - 1;
        memcpy(rawt, tid->name, lt);
        rawt[lt] = '\0';
  
        // build c_name = "<module>_<Enum>"
        char cnamet[256];
        size_t modlen = strlen(safe_module_path);
        size_t max_rawt = sizeof(cnamet) - modlen - 2;
        if (max_rawt > 0) {
          snprintf(cnamet, sizeof(cnamet), "%s_%.*s", safe_module_path, (int)max_rawt,
                   rawt);
          for (char *p = cnamet; *p; p++) if (*p == '.') *p = '_';
        } else {
          // module_path is too long; just truncate
          memcpy(cnamet, safe_module_path, sizeof(cnamet) - 1);
          cnamet[sizeof(cnamet) - 1] = '\0';
        }
  
        Type *ety = type_simple(sema_arena, tid);
        sema_insert_global(rawt, cnamet, ety, d, false);
  
        // 2) Do not register the variants here → they get resolved via
        // current_return_type later.
        break;
      }
  
      case DECL_C_INCLUDE:
      case DECL_IMPORT:
      case DECL_EVAL_IMPORT:
      case DECL_DESTRUCT:
        // already inlined earlier or not top-level
        break;
        
      case DECL_TYPE_ALIAS: {
        // Evaluate the right-hand side using the comptime interpreter
        Id *id = d->as.type_alias_decl.name;
        char raw[256];
        int lt = id->length < (int)sizeof(raw) - 1 ? id->length : (int)sizeof(raw) - 1;
        memcpy(raw, id->name, lt);
        raw[lt] = '\0';
        
        char cname[256];
        size_t modlen = strlen(safe_module_path);
        size_t max_raw = sizeof(cname) - modlen - 2;
        if (max_raw > 0) {
          snprintf(cname, sizeof(cname), "%s_%.*s", safe_module_path, (int)max_raw, raw);
          for (char *p = cname; *p; p++) if (*p == '.') *p = '_';
        } else {
          memcpy(cname, safe_module_path, sizeof(cname) - 1);
          cname[sizeof(cname) - 1] = '\0';
        }
        
        // For Phase B, we evaluate the RHS right now. This requires parsing the AST.
        // If it's a direct type, we get EXPR_TYPE. If it's a function call returning a type,
        // we execute the CTFE engine.
        // NOTE: we need to link in comptime.h, which we will do shortly.
        const char *old_path = current_module_path;
        current_module_path = safe_module_path;
        
        sema_resolve_expr(d->as.type_alias_decl.expr);
        Expr* eval_rhs = comptime_evaluate_expr(sema_arena, d->as.type_alias_decl.expr, NULL);
        
        current_module_path = old_path;
        
        if (eval_rhs) {
             if (eval_rhs->kind == EXPR_ANON_STRUCT) {
                  // Register as a struct!
                  Type *sty = type_simple(sema_arena, id);
                  
                  Decl* struct_d = arena_push_aligned(sema_arena, Decl);
                  struct_d->kind = DECL_STRUCT;
                  struct_d->as.struct_decl.name = id;
                  struct_d->as.struct_decl.fields = eval_rhs->as.anon_struct_expr.fields;
                  
                  sema_insert_global(raw, cname, sty, struct_d, false);
                  
                  DeclList *new_node = decl_list(sema_arena, struct_d);
                  DeclList *tail = sema_decls;
                  while (tail && tail->next) tail = tail->next;
                  if (tail) tail->next = new_node;
                  else sema_decls = new_node;
                  
             } else if (eval_rhs->kind == EXPR_ANON_ENUM) {
                  // Register as an enum!
                  Type *ety = type_simple(sema_arena, id);
                  
                  Decl* enum_d = arena_push_aligned(sema_arena, Decl);
                  enum_d->kind = DECL_ENUM;
                  enum_d->as.enum_decl.type_name = id;
                  enum_d->as.enum_decl.variants = eval_rhs->as.anon_enum_expr.variants;
                  
                  sema_insert_global(raw, cname, ety, enum_d, false);
                  
                  DeclList *new_node = decl_list(sema_arena, enum_d);
                  DeclList *tail = sema_decls;
                  while (tail && tail->next) tail = tail->next;
                  if (tail) tail->next = new_node;
                  else sema_decls = new_node;
                  
             } else if (eval_rhs->kind == EXPR_TYPE) {
                  // It's just an alias to an existing type (e.g., type MyInt = int)
                  sema_insert_global(raw, cname, eval_rhs->as.type_expr.type_value, d, false);
                  // Cache the evaluated type on the decl so downstream
                  // passes (emit) can inspect the underlying type.
                  d->as.type_alias_decl.expr = eval_rhs;
             } else {
                  fprintf(stderr, "[E012] Error Ln %li, Col %li: Type alias must evaluate to a type at compile-time (got kind=%d)\n", d->line, d->col, eval_rhs->kind);
                  diagnostic_show_line(d->line, d->col);
                  exit(1);
             }
        } else {
             fprintf(stderr, "[E012] Error Ln %li, Col %li: Type alias RHS could not be evaluated\n", d->line, d->col);
             diagnostic_show_line(d->line, d->col);
             exit(1);
        }
        break;
      }
      }
    }
    free(safe_module_path);
  }
  

/*
    name-resolution logic
*/

void sema_resolve_stmt(Stmt *s) {
  if (!s)
    return;
  switch (s->kind) {
  case STMT_USE: {
    Expr *target = s->as.use_stmt.target;
    sema_resolve_expr(target);
    sema_infer_expr(target);

    // alias:
    Id *alias = s->as.use_stmt.alias_name;
    char raw[256];
    memcpy(raw, alias->name, alias->length);
    raw[alias->length] = '\0';

    // fully qualified C name:
    char cname[256];
    sema_build_path(target, cname, sizeof(cname));
    for (char *p = cname; *p; p++) if (*p == '.') *p = '_';

    if (!target->type) {
      fprintf(stderr, "sema error: use-target `%s` has no type\n", cname);
      exit(1);
    }

    sema_insert_local(raw, cname, target->type, NULL, false);
    break;
  }

  case STMT_VAR: {
    // 1) resolve & infer initializer
    // 1) resolve & infer initializer
    Expr *rhs = s->as.var_stmt.expr;
    // Resolve a generic type-application annotation (`var v Vec(i32)`).
    if (s->as.var_stmt.type)
        s->as.var_stmt.type = mono_resolve_type_apps(s->as.var_stmt.type);
    Type *ty = s->as.var_stmt.type; // Start with the annotation (if any)
    
    // If there is an annotation, resolve it first
    if (ty) {
        // We assume types are already resolved or simple enough?
        // Actually, we might need to resolve the type name if it's a struct.
        // But the parser already creates a Type* node.
        // sema_resolve_type(ty); // If we had such a function.
        // For now, assume Type* from parser is valid or will be resolved by type_simple lookup if needed.
        // Wait, type_simple needs resolution?
        // sema_resolve_type is not defined in this file.
        // But `type_simple` stores `Id*`.
        // If we need to resolve it to a struct, we usually do it lazily or it's just a name.
    }

    if (rhs) {
      sema_resolve_expr(rhs);
      sema_infer_expr(rhs);
      if (sema_ranges) {
          // Range analysis moved to typecheck phase
      }
      if (!ty) {
          ty = rhs->type;           // infer from initializer
          // Strip MODE_MUTABLE: `var x = var_param` gives value type, not reference.
          // Exception: TYPE_POINTER with MODE_MUTABLE is a mutable thin pointer
          // (from `&arr[k]`) — preserve mutability so it emits without const.
          if (ty && ty->mode == MODE_MUTABLE && ty->kind != TYPE_POINTER) {
              Type *stripped = arena_push_aligned(sema_arena, Type);
              *stripped = *ty;
              stripped->mode = MODE_SHARED;
              ty = stripped;
          }
          s->as.var_stmt.type = ty;
      } else {
          // Function-pointer target: enforce arity / param+return types / totality.
          // (General value type-compat between ty and rhs->type is still TODO.)
          if (ty && ty->kind == TYPE_FUNC)
              fnptr_assign_check(ty, rhs, s->line, s->col);
          // SIMD vector target from an array literal: retype the literal to the
          // vector so it lowers to `(Vec_N_T){...}` (a GCC vector init), not a
          // Fixed_ struct. Enforce the lane count.
          if (ty && ty->kind == TYPE_VECTOR && rhs->kind == EXPR_ARRAY_LITERAL) {
              isize n = 0;
              for (ExprList *el = rhs->as.array_literal_expr.elements; el; el = el->next) n++;
              if (n != ty->array_len) {
                  fprintf(stderr, "[E012] Error Ln %li, Col %li: Vec(%ld, ...) needs %ld "
                          "lane values, got %ld.\n", s->line, s->col,
                          (long)ty->array_len, (long)ty->array_len, (long)n);
                  diagnostic_show_line(s->line, s->col); exit(1);
              }
              rhs->type = ty;
          }
      }
    }

    // 2) register the local variable
    Id *id = s->as.var_stmt.name;
    char raw[256];
    int L =
        id->length < (int)sizeof(raw) - 1 ? id->length : (int)sizeof(raw) - 1;
    memcpy(raw, id->name, L);
    raw[L] = '\0';

    if (sema_lookup(raw)) {
        fprintf(stderr, "[E013] Error Ln %li, Col %li: Redeclaration or shadowing of variable '%s' is forbidden\n", s->line, s->col, raw);
        diagnostic_show_line(s->line, s->col);
        exit(1);
    }

    const char *cname = raw;
    sema_insert_local(raw, cname, ty, NULL, s->as.var_stmt.is_mutable);
    break;
  }

  case STMT_IF: {
    // 1) Resolve + infer the condition expression
    Expr *cond = s->as.if_stmt.cond;
    sema_resolve_expr(cond);
    sema_infer_expr(cond);
    // 2) Recurse into the "then" branch (block-scoped)
    sema_push_scope();
    for (StmtList *b = s->as.if_stmt.then_body; b; b = b->next) {
      sema_resolve_stmt(b->stmt);
    }
    sema_pop_scope();
    // 3) If there's an "else" branch, recurse into it as well (block-scoped)
    sema_push_scope();
    for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next) {
      sema_resolve_stmt(b->stmt);
    }
    sema_pop_scope();
    break;
  }

  case STMT_FOR: {
    Expr *it = s->as.for_stmt.iterable;
    sema_resolve_expr(it);
    sema_infer_expr(it);

    Type *iter_ty = it->type;
    Type *idx_ty = get_builtin_i32_type();
    Type *val_ty = NULL;

    if (it->kind == EXPR_RANGE) {
        val_ty = get_builtin_i32_type();
    } else {
        assert(iter_ty &&
               (iter_ty->kind == TYPE_ARRAY || iter_ty->kind == TYPE_SLICE));
        val_ty = iter_ty->element_type;
    }

    // index variable (e.g. “i”)
    if (s->as.for_stmt.index_name) {
      char raw_i[256];
      Id *id_i = s->as.for_stmt.index_name;
      size_t cap_i = sizeof(raw_i) - 1;
      size_t len_i = (id_i->length < 0) ? 0 : (size_t)id_i->length;
      size_t li = len_i < cap_i ? len_i : cap_i;
      memcpy(raw_i, id_i->name, li);
      raw_i[li] = '\0';
      sema_insert_local(raw_i, raw_i, idx_ty, NULL, false);
      // Range Analysis: Set range for loop index
      if (sema_ranges && it->kind == EXPR_RANGE) {
          // Range analysis moved to typecheck phase
      }
    }

    // value variable (e.g. “c”)
    {
      char raw_c[256];
      Id *id_c = s->as.for_stmt.value_name;
      size_t cap_c = sizeof(raw_c) - 1;
      size_t len_c = (id_c->length < 0) ? 0 : (size_t)id_c->length;
      size_t lc = len_c < cap_c ? len_c : cap_c;
      memcpy(raw_c, id_c->name, lc);
      raw_c[lc] = '\0';
      sema_insert_local(raw_c, raw_c, val_ty, NULL, false);
    }

    // recurse into the loop body (block-scoped)
    sema_push_scope();
    for (StmtList *b = s->as.for_stmt.body; b; b = b->next) {
      sema_resolve_stmt(b->stmt);
    }
    sema_pop_scope();
    break;
  }

  case STMT_ASSIGN: {
    Expr *lhs = s->as.assign_stmt.target;
    Expr *rhs = s->as.assign_stmt.expr;

    // Implicit immutable declaration: bare `name = expr` where `name`
    // is not yet declared creates an immutable binding (type inferred).
    // If `name` IS declared, this is a reassignment (requires `var`).
    if (lhs->kind == EXPR_IDENTIFIER) {
      char raw[256];
      int L = lhs->as.identifier_expr.id->length;
      if (L >= (int)sizeof(raw))
        L = sizeof(raw) - 1;
      memcpy(raw, lhs->as.identifier_expr.id->name, L);
      raw[L] = '\0';

      Symbol *sym = sema_lookup(raw);
      if (!sym) {
        // Convert STMT_ASSIGN → STMT_VAR (immutable, type inferred from RHS)
        Id *name = lhs->as.identifier_expr.id;
        Expr *init = rhs;
        s->kind = STMT_VAR;
        s->as.var_stmt.name = name;
        s->as.var_stmt.type = NULL;
        s->as.var_stmt.expr = init;
        s->as.var_stmt.is_mutable = false;
        sema_resolve_stmt(s);
        return;
      } else if (!sym->is_mutable) {
        // Exception: var T parameter (mutable borrow) — assignment is write-through,
        // not rebind. The caller's value is modified via the pointer.
        bool is_var_param = sym->decl &&
                            sym->decl->kind == DECL_VARIABLE &&
                            sym->decl->as.variable_decl.is_parameter &&
                            sym->decl->as.variable_decl.type &&
                            sym->decl->as.variable_decl.type->mode == MODE_MUTABLE;
        if (!is_var_param) {
          fprintf(stderr, "[E009] Error Ln %li, Col %li: Cannot assign to immutable variable '%s'. "
                  "Declare with 'var' for mutable variables.\n",
                  s->line, s->col, raw);
          diagnostic_show_line(s->line, s->col);
          exit(1);
        }
      }
    }

    // Otherwise, it's a normal assignment: resolve/mangle both sides
    sema_resolve_expr(lhs);
    sema_resolve_expr(rhs);
    sema_infer_expr(lhs);
    sema_infer_expr(rhs);

    // Range Analysis: Update range
    if (sema_ranges && lhs->kind == EXPR_IDENTIFIER) {
        // Range analysis moved to typecheck phase
    }

    // Purity Check: func cannot modify global variable
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        if (lhs->is_global && lhs->decl && lhs->decl->kind == DECL_VARIABLE) {
             fprintf(stderr, "[E011] Error Ln %li, Col %li: Pure function '%.*s' cannot modify global variable\n",
                     s->line, s->col,
                        (int)current_function_decl->as.function_decl.name->length,
                        current_function_decl->as.function_decl.name->name);
             diagnostic_show_line(s->line, s->col);
             exit(1);
        }
    }
    break;
  }

  case STMT_EXPR:
    sema_resolve_expr(s->as.expr_stmt.expr);
    break;

  case STMT_RETURN:
    sema_resolve_expr(s->as.return_stmt.value);
    // Ban returning mutable reference to local variables
    // Recursively unwrap EXPR_MEMBER and EXPR_INDEX to find the root identifier.
    if (s->as.return_stmt.value && s->as.return_stmt.value->kind == EXPR_MUT) {
        Expr *root = s->as.return_stmt.value->as.mut_expr.expr;
        while (root) {
            if (root->kind == EXPR_MEMBER) root = root->as.member_expr.target;
            else if (root->kind == EXPR_INDEX) root = root->as.index_expr.target;
            else break;
        }
        if (root && root->kind == EXPR_IDENTIFIER && !root->is_global) {
            // Allow return var for parameters (their data outlives the function)
            bool is_param = false;
            if (root->decl && root->decl->kind == DECL_VARIABLE) {
                is_param = root->decl->as.variable_decl.is_parameter;
            }
            if (!is_param) {
                fprintf(stderr, "[E010] Error Ln %li, Col %li: Returning a mutable reference ('var') to a local variable is forbidden (dangling pointer)\n", s->line, s->col);
                diagnostic_show_line(s->line, s->col);
                exit(1);
            }
        }
    }
    break;

  case STMT_MATCH:
    sema_resolve_expr(s->as.match_stmt.value);
    for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
      sema_push_scope();
      for (ExprList *p = c->patterns; p; p = p->next) {
        sema_resolve_expr(p->expr);
      }
      for (StmtList *b = c->body; b; b = b->next) {
        sema_resolve_stmt(b->stmt);
      }
      sema_pop_scope();
    }
    // Check exhaustiveness after resolving all cases
    if (!sema_check_match_exhaustive(s)) {
      sema_report_nonexhaustive_match(s);
      exit(1);
    }
    break;
  
  case STMT_WHILE: {
    // Purity: while loops without a termination measure are banned in pure functions,
    // UNLESS the condition consists entirely of pointer-in-arr guards (p in arr where
    // p has TYPE_POINTER): the walk phase will auto-synthesize the measure from the
    // monotone pointer decrement pattern.
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        if (!s->as.while_stmt.measure) {
            // Structural scan: any `expr in expr` in condition → defer to walk phase
            bool has_in_cond = false;
            {
                Expr *stk[16]; int top = 0; stk[top++] = s->as.while_stmt.cond;
                while (top > 0) {
                    Expr *e = stk[--top];
                    if (!e || e->kind != EXPR_BINARY) continue;
                    if (e->as.binary_expr.op == TOKEN_KEYWORD_IN) { has_in_cond = true; break; }
                    if (e->as.binary_expr.op == TOKEN_KEYWORD_AND && top < 14) {
                        stk[top++] = e->as.binary_expr.left;
                        stk[top++] = e->as.binary_expr.right;
                    }
                }
            }
            if (!has_in_cond) {
                fprintf(stderr, "[E011] Error Ln %li, Col %li: 'while' loops without a termination measure "
                        "are not allowed in pure function '%.*s'. "
                        "Add 'decreasing <measure>' or use 'proc'.\n",
                        s->line, s->col,
                        (int)current_function_decl->as.function_decl.name->length,
                        current_function_decl->as.function_decl.name->name);
                diagnostic_show_line(s->line, s->col);
                exit(1);
            }
            // has_in_cond: defer — walk phase will auto-infer or emit E011
        }
    }
    // Resolve condition, measure, and body
    sema_resolve_expr(s->as.while_stmt.cond);
    if (s->as.while_stmt.measure) {
        sema_resolve_expr(s->as.while_stmt.measure);
    }
    sema_push_scope();
    for (StmtList *b = s->as.while_stmt.body; b; b = b->next) {
        sema_resolve_stmt(b->stmt);
    }
    sema_pop_scope();
    break;
  }

 case STMT_UNSAFE: {
    bool old = sema_in_unsafe_block;
    sema_in_unsafe_block = true;
    sema_push_scope();
    for (StmtList *b = s->as.unsafe_stmt.body; b; b = b->next) {
        sema_resolve_stmt(b->stmt);
    }
    sema_pop_scope();
    sema_in_unsafe_block = old;
    break;
 }

 case STMT_DEFER:
    sema_resolve_stmt(s->as.defer_stmt.stmt);
    break;

  case STMT_COMPTIME_IF: {
    // 1) Resolve the condition expression (it may contain @os, @arch, etc.)
    Expr *cond = s->as.comptime_if_stmt.cond;
    sema_resolve_expr(cond);

    // 2) Evaluate the condition at compile time
    Expr *eval = comptime_evaluate_expr(sema_arena, cond, NULL);
    bool is_true = false;
    if (eval && eval->kind == EXPR_LITERAL) {
        is_true = eval->as.literal_expr.value != 0;
    } else {
        fprintf(stderr, "[E014] Error Ln %li, Col %li: comptime if condition must evaluate to a compile-time constant\n",
                s->line, s->col);
        diagnostic_show_line(s->line, s->col);
        exit(1);
    }

    // 3) Mark which branch was taken
    s->as.comptime_if_stmt.evaluated = true;
    s->as.comptime_if_stmt.is_taken = is_true;

    // 4) Only resolve the taken branch (dead branch is parsed but not resolved)
    if (is_true) {
        sema_push_scope();
        for (StmtList *b = s->as.comptime_if_stmt.then_body; b; b = b->next) {
            sema_resolve_stmt(b->stmt);
        }
        sema_pop_scope();
    } else if (s->as.comptime_if_stmt.else_branch) {
        sema_push_scope();
        for (StmtList *b = s->as.comptime_if_stmt.else_branch; b; b = b->next) {
            sema_resolve_stmt(b->stmt);
        }
        sema_pop_scope();
    }
    break;
  }

  default:
    break;
  }
}

void sema_resolve_expr(Expr *e) {
  if (!e)
    return;
  switch (e->kind) {
  case EXPR_IDENTIFIER: {
    // 1) get the raw text from the AST node
    char raw[256];
    int L = e->as.identifier_expr.id->length;
    if (L >= (int)sizeof(raw))
      L = sizeof(raw) - 1;
    memcpy(raw, e->as.identifier_expr.id->name, L);
    raw[L] = '\0';

    // 2) lookup in the two‐table (locals first, then globals)
    Symbol *sym = sema_lookup(raw);
    if (sym) {
      // Q-018: enforce [private] cross-module visibility using defining_module
      // tag set by load_module().
      if (sym->is_global && sym->decl && sym->decl->is_private && current_module_path
          && sym->decl->defining_module) {
        if (strcmp(sym->decl->defining_module, current_module_path) != 0) {
          fprintf(stderr, "[E084] Error Ln %li, Col %li: cannot access private declaration '%.*s' from module '%s' (defined in '%s')\n",
                  e->line, e->col,
                  (int)e->as.identifier_expr.id->length, e->as.identifier_expr.id->name,
                  current_module_path, sym->decl->defining_module);
          exit(1);
        }
      }

      // Glob retirement: a bare name from ANOTHER module is visible only if it was
      // selectively imported (`import M.{name}`) or reached qualified (`M.name`).
      // The whole `import M` grants qualified access, not bare names.
      if (sym->is_global && sym->decl && current_module_path && sym->decl->defining_module
          && !e->as.identifier_expr.via_qualifier
          && !module_paths_equal(sym->decl->defining_module, current_module_path)
          && !sel_import_visible(current_module_path, raw, (size_t)L)) {
        const char *seg = strrchr(sym->decl->defining_module, '.');
        seg = seg ? seg + 1 : sym->decl->defining_module;
        fprintf(stderr, "[E105] Error Ln %li, Col %li: '%.*s' is defined in module '%s' — "
                "import it (`import %s.{%.*s}`) or qualify it (`%s.%.*s`).\n",
                e->line, e->col,
                (int)e->as.identifier_expr.id->length, e->as.identifier_expr.id->name,
                sym->decl->defining_module,
                sym->decl->defining_module, (int)e->as.identifier_expr.id->length, e->as.identifier_expr.id->name,
                seg, (int)e->as.identifier_expr.id->length, e->as.identifier_expr.id->name);
        exit(1);
      }

      if (sym->decl && (sym->decl->kind == DECL_STRUCT || sym->decl->kind == DECL_ENUM || sym->decl->kind == DECL_EXTERN_TYPE)) {
          // It's a user-defined type!
          e->kind = EXPR_TYPE;
          e->as.type_expr.type_value = sym->type;
          e->type = NULL;
          e->decl = sym->decl;
          e->is_global = true;
          break;
      }

      // Instead of pointing at sym->c_name (which may get freed),
      // copy the string into the permanent arena:
      const char *mangled = sym->c_name;
      size_t mlen = strlen(mangled);

      // Allocate (mlen+1) bytes in sema_arena and copy there
      char *copy = arena_push_many_aligned(sema_arena, char, mlen + 1);
      memcpy(copy, mangled, mlen + 1); // include the '\0'

      // Now point the AST’s identifier at the arena‐allocated copy:
      e->as.identifier_expr.id->name = copy;
      e->as.identifier_expr.id->length = (isize)mlen;
      e->type = sym->type;
      e->decl = sym->decl;       // Populate decl
      e->is_global = sym->is_global; // Populate is_global
      break;
    }

    // 3) Q-014/G-007: 'panic' builtin — recognized identifier, type Never (any)
    if (strcmp(raw, "panic") == 0) {
        // mark as builtin: leave kind=EXPR_IDENTIFIER but ensure type is settable
        // codegen will recognize callee identifier "panic" specially
        e->type = get_builtin_i32_type();  // 'Never'-style: callable, return type irrelevant
        e->is_global = true;
        break;
    }

    // 3) fallback: maybe it’s a builtin type name?
    // Sized integers iN/uN (N=1..64) are builtin type names.
    {
        size_t rl = strlen(raw);
        if (rl >= 2 && rl <= 3 && (raw[0] == 'i' || raw[0] == 'u')) {
            bool all_digits = true;
            int bits = 0;
            for (size_t k = 1; k < rl; k++) {
                if (raw[k] < '0' || raw[k] > '9') { all_digits = false; break; }
                bits = bits * 10 + (raw[k] - '0');
            }
            if (all_digits && bits >= 1 && bits <= 64) {
                Id *type_id = arena_push_aligned(sema_arena, Id);
                char *nbuf = arena_push_many_aligned(sema_arena, char, rl + 1);
                memcpy(nbuf, raw, rl + 1);
                type_id->name = nbuf;
                type_id->length = (isize)rl;
                e->kind = EXPR_TYPE;
                e->as.type_expr.type_value = type_simple(sema_arena, type_id);
                e->type = NULL;
                break;
            }
        }
    }
    // `int` and `float` are documented aliases of i32 and f32.
    if (strcmp(raw, "int") == 0) {
        e->kind = EXPR_TYPE;
        e->as.type_expr.type_value = get_builtin_i32_type();
        e->type = NULL;
        break;
    }
    if (strcmp(raw, "u8") == 0) {
        e->kind = EXPR_TYPE;
        e->as.type_expr.type_value = get_builtin_u8_type();
        e->type = NULL;
        break;
    } else if (strcmp(raw, "f32") == 0 || strcmp(raw, "f64") == 0 || strcmp(raw, "float") == 0 || strcmp(raw, "bool") == 0 || strcmp(raw, "string") == 0) {
        e->kind = EXPR_TYPE;
        Id *type_id = id(sema_arena, strlen(raw), arena_push_many_aligned(sema_arena, char, strlen(raw) + 1));
        strcpy((char*)type_id->name, raw);
        e->as.type_expr.type_value = type_simple(sema_arena, type_id);
        e->type = NULL;
        break;
    }

    // 4) fallback: maybe it’s an enum‐variant …
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
      Decl *D = dl->decl;
      if (D && D->kind == DECL_ENUM) {
        Id *enum_id = D->as.enum_decl.type_name;
        for (Variant *vl = D->as.enum_decl.variants; vl; vl = vl->next) {
          Id *vid = vl->name;
          if ((size_t)vid->length == strlen(raw) &&
              strncmp(vid->name, raw, vid->length) == 0) {
            // build "<module>_<Enum>_<Variant>"
            static char buf[512];
             snprintf(buf, sizeof(buf), "%s_%.*s_%.*s", current_module_path,
                      (int)enum_id->length, enum_id->name, (int)vid->length,
                      vid->name);
            // Sanitize dots in the generated name
            for (char *p = buf; *p; p++) {
                if (*p == '.') *p = '_';
            }

            size_t buflen = strlen(buf) + 1;
            char *copy = arena_push_many_aligned(sema_arena, char, buflen);
            memcpy(copy, buf, buflen);

            e->as.identifier_expr.id->name = copy;
            e->as.identifier_expr.id->length = (isize)strlen(copy);
            e->type = get_builtin_i32_type();
            e->decl = D; // Enum variant belongs to Enum Decl
            e->is_global = true;
            return;
          }
        }
      }
    }

    // 4) leave unresolved (will be an error later)
    break;
  }

  case EXPR_MEMBER: {
    Expr *mtgt = e->as.member_expr.target;
    // Qualified module access: `qualifier.Member`, where `qualifier` is an
    // imported module or its alias. The glob already binds the member's bare
    // name, so rewrite this node into that identifier and resolve it normally.
    if (mtgt && mtgt->kind == EXPR_IDENTIFIER && e->as.member_expr.member) {
        Id *q = mtgt->as.identifier_expr.id;
        if (q && qualifier_is_module(q->name, (size_t)q->length)) {
            Id *member = e->as.member_expr.member;
            e->kind = EXPR_IDENTIFIER;
            e->as.identifier_expr.id = member;
            e->as.identifier_expr.via_qualifier = true;   // exempt from glob-retirement
            sema_resolve_expr(e);
            break;
        }
    }
    sema_resolve_expr(mtgt);
    break;
  }
  case EXPR_BINARY:
    sema_resolve_expr(e->as.binary_expr.left);
    sema_resolve_expr(e->as.binary_expr.right);
    break;
  case EXPR_UNARY:
    sema_resolve_expr(e->as.unary_expr.right);
    break;
  case EXPR_CALL:
    sema_resolve_expr(e->as.call_expr.callee);

    // Resolve arguments first (so a type argument like `i32` becomes EXPR_TYPE),
    // then rewrite a generic call to its concrete monomorphized instance.
    for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
      sema_resolve_expr(a->expr);
    }
    sema_monomorphize_call(e);   // no-op unless callee is a generic template

    // Purity Check: func cannot call proc (checked on the possibly-rewritten callee)
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        Expr *callee = e->as.call_expr.callee;
        if (callee->decl) {
            if (callee->decl->kind == DECL_PROCEDURE || callee->decl->kind == DECL_EXTERN_PROCEDURE) {
                fprintf(stderr, "[E011] Error Ln %li, Col %li: Pure function '%.*s' cannot call procedure\n",
                        e->line, e->col,
                        (int)current_function_decl->as.function_decl.name->length,
                        current_function_decl->as.function_decl.name->name);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
    }
    break;
  case EXPR_RANGE:
    sema_resolve_expr(e->as.range_expr.start);
    sema_resolve_expr(e->as.range_expr.end);
    break;
  case EXPR_INDEX:
    sema_resolve_expr(e->as.index_expr.target);
    sema_resolve_expr(e->as.index_expr.index);
    break;
  case EXPR_ADDR:
    sema_resolve_expr(e->as.addr_expr.expr);
    break;

  case EXPR_DEREF:
    sema_resolve_expr(e->as.deref_expr.expr);
    break;

  case EXPR_MOVE:
    sema_resolve_expr(e->as.move_expr.expr);
    break;

  case EXPR_MUT:
    sema_resolve_expr(e->as.mut_expr.expr);
    break;

  case EXPR_CAST:
    sema_resolve_expr(e->as.cast_expr.expr);
    break;

  case EXPR_MATCH:
    sema_resolve_expr(e->as.match_expr.value);
    for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
        for (ExprList *p = c->patterns; p; p = p->next) {
            sema_resolve_expr(p->expr);
        }
        sema_resolve_expr(c->body);
    }
    break;

  case EXPR_ARRAY_LITERAL:
    for (ExprList *el = e->as.array_literal_expr.elements; el; el = el->next) {
        sema_resolve_expr(el->expr);
    }
    break;

  case EXPR_ARRAY_COMPREHENSION: {
    // [ body for idx in range ] — resolve the range, then bind idx (an i32) in a
    // fresh scope while resolving the body.
    sema_resolve_expr(e->as.array_comprehension_expr.range);
    sema_infer_expr(e->as.array_comprehension_expr.range);
    sema_push_scope();
    Id *ix = e->as.array_comprehension_expr.idx;
    if (ix) {
        char raw[256];
        size_t li = (ix->length < 0) ? 0 : (size_t)ix->length;
        if (li > sizeof(raw) - 1) li = sizeof(raw) - 1;
        memcpy(raw, ix->name, li); raw[li] = '\0';
        sema_insert_local(raw, raw, get_builtin_i32_type(), NULL, false);
    }
    sema_resolve_expr(e->as.array_comprehension_expr.body);
    sema_infer_expr(e->as.array_comprehension_expr.body);   // set body type while idx is in scope
    sema_pop_scope();
    break;
  }

  case EXPR_BUILTIN: {
    switch (e->as.builtin_expr.builtin_kind) {
        case BUILTIN_OS:
        case BUILTIN_ARCH: {
            // Resolve @os / @arch to compile-time integer literals
            int value = (e->as.builtin_expr.builtin_kind == BUILTIN_OS)
                        ? LAIN_TARGET_OS : LAIN_TARGET_ARCH;
            e->kind = EXPR_LITERAL;
            e->as.literal_expr.value = value;
            e->type = get_builtin_i32_type();
            break;
        }
        case BUILTIN_LIKELY:
        case BUILTIN_UNLIKELY:
        case BUILTIN_ASSUME_ALIGNED:
            // Resolve the inner argument, keep node as-is
            if (e->as.builtin_expr.arg)
                sema_resolve_expr(e->as.builtin_expr.arg);
            break;
    }
    break;
  }

  default:
    break;
  }
}

#endif /* SEMA_RESOLVE_H */
