// resolve.h : name-resolution logic

#ifndef SEMA_RESOLVE_H
#define SEMA_RESOLVE_H


#include "../ast.h"
#include "../ast_clone.h"
#include "generic.h"
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
                  // passes (niche, emit) can inspect the underlying type.
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
          // TODO: check compatibility between ty and rhs->type
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
        s->as.var_stmt.explicit_undefined = false;
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
    // Purity: while loops without a termination measure are banned in pure functions
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        if (!s->as.while_stmt.measure) {
            fprintf(stderr, "[E011] Error Ln %li, Col %li: 'while' loops without a termination measure "
                    "are not allowed in pure function '%.*s'. "
                    "Add 'decreasing <measure>' or use 'proc'.\n",
                    s->line, s->col,
                    (int)current_function_decl->as.function_decl.name->length,
                    current_function_decl->as.function_decl.name->name);
            diagnostic_show_line(s->line, s->col);
            exit(1);
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

      if (sym->decl && (sym->decl->kind == DECL_STRUCT || sym->decl->kind == DECL_ENUM || sym->decl->kind == DECL_EXTERN_TYPE)) {
          // It's a user-defined type!
          e->kind = EXPR_TYPE;
          e->as.type_expr.type_value = sym->type;
          e->type = type_meta_type(sema_arena);
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
                e->type = type_meta_type(sema_arena);
                break;
            }
        }
    }
    // `int` and `float` are documented aliases of i32 and f32.
    if (strcmp(raw, "int") == 0) {
        e->kind = EXPR_TYPE;
        e->as.type_expr.type_value = get_builtin_i32_type();
        e->type = type_meta_type(sema_arena);
        break;
    }
    if (strcmp(raw, "u8") == 0) {
        e->kind = EXPR_TYPE;
        e->as.type_expr.type_value = get_builtin_u8_type();
        e->type = type_meta_type(sema_arena);
        break;
    } else if (strcmp(raw, "f32") == 0 || strcmp(raw, "f64") == 0 || strcmp(raw, "float") == 0 || strcmp(raw, "bool") == 0 || strcmp(raw, "string") == 0) {
        e->kind = EXPR_TYPE;
        Id *type_id = id(sema_arena, strlen(raw), arena_push_many_aligned(sema_arena, char, strlen(raw) + 1));
        strcpy((char*)type_id->name, raw);
        e->as.type_expr.type_value = type_simple(sema_arena, type_id);
        e->type = type_meta_type(sema_arena);
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

  case EXPR_MEMBER:
    sema_resolve_expr(e->as.member_expr.target);
    break;
  case EXPR_BINARY:
    sema_resolve_expr(e->as.binary_expr.left);
    sema_resolve_expr(e->as.binary_expr.right);
    break;
  case EXPR_UNARY:
    sema_resolve_expr(e->as.unary_expr.right);
    break;
  case EXPR_CALL:
    sema_resolve_expr(e->as.call_expr.callee);

    // Note: Q-002 Phase 3 (int monomorphization for i64/u64) was reverted.
    // Rationale: it introduced hidden polymorphism that violated P5
    // "no hidden magic". `int` is now a stable alias for i32; if you
    // want a function that accepts wider integers, declare them
    // explicitly: `func f(n i64)` or use type parameters `func f(T type, n T)`.

    // Check if the callee resolved to a generic func
    if (e->as.call_expr.callee->decl) {
        Decl *df = e->as.call_expr.callee->decl;
        if (df->kind == DECL_FUNCTION || df->kind == DECL_PROCEDURE) {
            // Check for comptime parameters
            bool is_generic = false;
            for (DeclList *p = df->as.function_decl.params; p; p = p->next) {
                if (p->decl && p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.type && p->decl->as.variable_decl.type->kind == TYPE_COMPTIME) {
                    is_generic = true;
                    break;
                }
            }

            if (is_generic) {
                // 1. Resolve arguments first to get types
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) sema_resolve_expr(a->expr);

                // 2. Build mangled name, e.g., max_int
                char mangled_name[512];
                snprintf(mangled_name, sizeof(mangled_name), "%.*s", (int)df->as.function_decl.name->length, df->as.function_decl.name->name);
                
                DeclList *p = df->as.function_decl.params;
                ExprList *a = e->as.call_expr.args;
                while (p && a) {
                    if (p->decl && p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.type && p->decl->as.variable_decl.type->kind == TYPE_COMPTIME) {
                        if (a->expr->kind == EXPR_TYPE) {
                            Type *arg_ty = a->expr->as.type_expr.type_value;
                            if (arg_ty->kind == TYPE_SIMPLE && arg_ty->base_type) {
                                snprintf(mangled_name + strlen(mangled_name), sizeof(mangled_name) - strlen(mangled_name), "_%.*s", 
                                    (int)arg_ty->base_type->length, arg_ty->base_type->name);
                            } else {
                                snprintf(mangled_name + strlen(mangled_name), sizeof(mangled_name) - strlen(mangled_name), "_complexT");
                            }
                        } else {
                             fprintf(stderr, "[E012] Error Ln %li: call to generic function '%.*s' missing type argument\n",
                                 e->line, (int)df->as.function_decl.name->length, df->as.function_decl.name->name);
                             diagnostic_show_line(e->line, e->col);
                             exit(1);
                        }
                    }
                    p = p->next;
                    a = a->next;
                }

                size_t cname_mangled_len = strlen(current_module_path) + 1 + strlen(mangled_name) + 1;
                char *cname_mangled = malloc(cname_mangled_len);
                snprintf(cname_mangled, cname_mangled_len, "%s_%s", current_module_path, mangled_name);
                for (char *ptr = cname_mangled; *ptr; ptr++) if (*ptr == '.') *ptr = '_';

                // 3. Instantiate if needed
                Symbol *existing = sema_lookup(mangled_name);
                if (!existing) {
                    Decl *inst = clone_decl(sema_arena, df);
                    
                    // Replace the name
                    inst->as.function_decl.name = id(sema_arena, strlen(mangled_name), arena_push_many_aligned(sema_arena, char, strlen(mangled_name) + 1));
                    strcpy((char*)inst->as.function_decl.name->name, mangled_name);

                    // Substitute type parameters
                    p = inst->as.function_decl.params;
                    a = e->as.call_expr.args;
                    while (p && a) {
                        if (p->decl && p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.type && p->decl->as.variable_decl.type->kind == TYPE_COMPTIME) {
                            Id *param_name = p->decl->as.variable_decl.name;
                            Type *arg_ty = a->expr->as.type_expr.type_value;
                            p->decl->as.variable_decl.type = type_meta_type(sema_arena);
                            
                            char param_str[256];
                            snprintf(param_str, 256, "%.*s", (int)param_name->length, param_name->name);
                            generic_substitute_decl(inst, param_str, arg_ty);
                        }
                        p = p->next;
                        a = a->next;
                    }

                    sema_insert_global(mangled_name, cname_mangled, inst->as.function_decl.return_type, inst, false);
                    
                    // Append to sema_decls so it will be typechecked and emitted
                    DeclList *new_node = decl_list(sema_arena, inst);
                    DeclList *tail = sema_decls;
                    while (tail && tail->next) tail = tail->next;
                    if (tail) tail->next = new_node;
                    else sema_decls = new_node;
                }
                
                // 4. Update the callee identifier to point to `mangled_name`
                e->as.call_expr.callee->as.identifier_expr.id = id(sema_arena, strlen(mangled_name), arena_push_many_aligned(sema_arena, char, strlen(mangled_name) + 1));
                strcpy((char*)e->as.call_expr.callee->as.identifier_expr.id->name, mangled_name);
                
                // Re-resolve callee
                e->as.call_expr.callee->decl = NULL;
                sema_resolve_expr(e->as.call_expr.callee);
                
                free(cname_mangled);
                break; // Skip normal arg resolution, we already did it
            }
        }
    }

    // 'T type variable generics: infer type bindings from argument types.
    // Unlike comptime T type, no explicit type arg — T is inferred from values.
    if (e->as.call_expr.callee->decl) {
        Decl *df = e->as.call_expr.callee->decl;
        if (df->kind == DECL_FUNCTION || df->kind == DECL_PROCEDURE) {
            // Helper: does this type contain any 'T or 'N (typevar or typevar-sized array)?
            #define type_has_typevar(t) ({                                          \
                Type *_t = (t); bool _r = false;                                   \
                while (_t) {                                                        \
                    if (_t->kind == TYPE_VAR) { _r = true; break; }                \
                    if (_t->kind == TYPE_ARRAY && _t->size_relop == TOKEN_TYPEVAR)  \
                        { _r = true; break; }                                      \
                    _t = _t->element_type;                                         \
                }                                                                   \
                _r; })
            bool is_typevar = false;
            for (DeclList *p = df->as.function_decl.params; p; p = p->next) {
                if (p->decl && p->decl->kind == DECL_VARIABLE) {
                    Type *pt = p->decl->as.variable_decl.type;
                    if (pt && type_has_typevar(pt)) { is_typevar = true; break; }
                }
            }
            if (is_typevar) {
                // 1. Resolve + infer all args to get concrete types
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    sema_resolve_expr(a->expr);
                    sema_infer_expr(a->expr);
                }
                // 2. Bind type variables ('T → Type) and size variables ('N → integer)
                #define MAX_TVARS 8
                const char *tv_names[MAX_TVARS]; Type *tv_types[MAX_TVARS];
                const char *sv_names[MAX_TVARS]; isize sv_sizes[MAX_TVARS];
                int n_tv = 0, n_sv = 0;
                DeclList *p = df->as.function_decl.params;
                ExprList *a = e->as.call_expr.args;
                while (p && a) {
                    if (p->decl && p->decl->kind == DECL_VARIABLE) {
                        Type *pt = p->decl->as.variable_decl.type;
                        Type *at = a->expr->type;
                        // Direct 'T type variable: param is TYPE_VAR
                        if (pt && pt->kind == TYPE_VAR && pt->base_type && at) {
                            bool found = false;
                            for (int i = 0; i < n_tv; i++) {
                                if (strncmp(tv_names[i], pt->base_type->name,
                                            pt->base_type->length) == 0) { found = true; break; }
                            }
                            if (!found && n_tv < MAX_TVARS) {
                                char *nm = arena_push_many_aligned(sema_arena, char,
                                                                   pt->base_type->length + 1);
                                memcpy(nm, pt->base_type->name, pt->base_type->length);
                                nm[pt->base_type->length] = '\0';
                                tv_names[n_tv] = nm; tv_types[n_tv] = at; n_tv++;
                            }
                        }
                        // Size variable 'N: param is *T['N] (POINTER to typevar-sized ARRAY)
                        // Arg is T[K] (inline fixed array) → bind 'N = K
                        if (pt && pt->kind == TYPE_POINTER && pt->element_type &&
                            pt->element_type->kind == TYPE_ARRAY &&
                            pt->element_type->array_len < 0 &&
                            pt->element_type->size_relop == TOKEN_TYPEVAR &&
                            pt->element_type->size_expr &&
                            pt->element_type->size_expr->kind == EXPR_IDENTIFIER &&
                            at && at->kind == TYPE_ARRAY && at->array_len >= 0) {
                            Id *sv_id = pt->element_type->size_expr->as.identifier_expr.id;
                            bool found = false;
                            for (int i = 0; i < n_sv; i++) {
                                if (strncmp(sv_names[i], sv_id->name, sv_id->length) == 0)
                                    { found = true; break; }
                            }
                            if (!found && n_sv < MAX_TVARS) {
                                char *nm = arena_push_many_aligned(sema_arena, char,
                                                                   sv_id->length + 1);
                                memcpy(nm, sv_id->name, sv_id->length);
                                nm[sv_id->length] = '\0';
                                sv_names[n_sv] = nm; sv_sizes[n_sv] = at->array_len; n_sv++;
                            }
                        }
                    }
                    p = p->next; a = a->next;
                }
                if (n_tv > 0 || n_sv > 0) {
                    // 3. Build mangled name: funcname_T1_T2..._N1_N2...
                    char mn[512];
                    snprintf(mn, sizeof(mn), "%.*s",
                             (int)df->as.function_decl.name->length,
                             df->as.function_decl.name->name);
                    for (int i = 0; i < n_tv; i++) {
                        char tn[128] = "T";
                        Type *bt = tv_types[i];
                        if (bt->kind == TYPE_SIMPLE && bt->base_type) {
                            snprintf(tn, sizeof(tn), "%.*s",
                                     (int)bt->base_type->length, bt->base_type->name);
                        }
                        snprintf(mn + strlen(mn), sizeof(mn) - strlen(mn), "_%s", tn);
                    }
                    for (int i = 0; i < n_sv; i++) {
                        snprintf(mn + strlen(mn), sizeof(mn) - strlen(mn),
                                 "_%lld", (long long)sv_sizes[i]);
                    }
                    // 4. Instantiate if not already done
                    size_t cmlen = strlen(current_module_path) + 1 + strlen(mn) + 1;
                    char *cm = malloc(cmlen);
                    snprintf(cm, cmlen, "%s_%s", current_module_path, mn);
                    for (char *ptr = cm; *ptr; ptr++) if (*ptr == '.') *ptr = '_';

                    if (!sema_lookup(mn)) {
                        Decl *inst = clone_decl(sema_arena, df);
                        size_t mlen = strlen(mn);
                        char *mbuf = arena_push_many_aligned(sema_arena, char, mlen + 1);
                        memcpy(mbuf, mn, mlen + 1);
                        inst->as.function_decl.name = id(sema_arena, mlen, mbuf);
                        // Substitute all type variables
                        for (int i = 0; i < n_tv; i++)
                            generic_substitute_decl(inst, tv_names[i], tv_types[i]);
                        // Substitute all size variables ('N → integer)
                        for (int i = 0; i < n_sv; i++)
                            generic_substitute_size_in_decl(inst, sv_names[i], sv_sizes[i]);
                        sema_insert_global(mn, cm, inst->as.function_decl.return_type, inst, false);
                        DeclList *nn = decl_list(sema_arena, inst);
                        DeclList *tail = sema_decls;
                        while (tail && tail->next) tail = tail->next;
                        if (tail) tail->next = nn; else sema_decls = nn;
                    }
                    // 5. Rewrite callee to mangled name
                    size_t mlen = strlen(mn);
                    char *mbuf2 = arena_push_many_aligned(sema_arena, char, mlen + 1);
                    memcpy(mbuf2, mn, mlen + 1);
                    e->as.call_expr.callee->as.identifier_expr.id = id(sema_arena, mlen, mbuf2);
                    e->as.call_expr.callee->decl = NULL;
                    sema_resolve_expr(e->as.call_expr.callee);
                    free(cm);
                    break; // args already resolved; skip normal resolution
                }
            }
        }
    }

    // Purity Check: func cannot call proc
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

    // Normal argument resolution for non-generic
    for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
      sema_resolve_expr(a->expr);
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

  case EXPR_BUILTIN: {
    // Resolve @os / @arch to compile-time integer literals
    int value = 0;
    switch (e->as.builtin_expr.builtin_kind) {
        case BUILTIN_OS:   value = LAIN_TARGET_OS;   break;
        case BUILTIN_ARCH: value = LAIN_TARGET_ARCH; break;
    }
    // Replace this node in-place with a literal
    e->kind = EXPR_LITERAL;
    e->as.literal_expr.value = value;
    e->type = get_builtin_i32_type();
    break;
  }

  default:
    break;
  }
}

#endif /* SEMA_RESOLVE_H */
