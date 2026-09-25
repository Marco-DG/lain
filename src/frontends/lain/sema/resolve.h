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

        // ⚠ `func main` USED TO BE REJECTED, requiring `proc main`. That rule belonged to the
        // two-keyword world: with one introducer (plan 7B) `main` is a `func` like everything
        // else, declaring `effects io` when it prints and nothing when it does not — and a `main`
        // that is genuinely pure and total is a perfectly good program, which the old rule
        // forbade for no reason it could state.
        //
        // What the rule was really protecting is unchanged and enforced elsewhere: `main`'s
        // effects still have to be ACKNOWLEDGED, so `func main()` that prints without
        // `effects io` is [E011]. The keyword was never what made that work.

        // Returning a FIXED-size array by value (`func f() i32[4]`) is not yet
        // supported and, left unchecked, emits broken C: the Fixed_T_N struct
        // return mismatches the `return <array/ptr>` expression, and the caller's
        // `int32_t b[4] = f()` array-initializer is illegal in C. Reject cleanly
        // (fail-closed) instead. Slice returns (`i32[n]`, array_len == -1, emitted
        // as a Slice_<T> struct) and `var` output parameters are the supported
        // ways to hand back array data.
        if ((d->kind == DECL_FUNCTION || d->kind == DECL_PROCEDURE) &&
            rt && rt->kind == TYPE_ARRAY && rt->array_len > 0) {
            fprintf(stderr, "[E088] Error Ln %li, Col %li: '%.*s' returns a fixed-size array "
                    "by value, which is not supported. Return a slice ('T[n]') or write the "
                    "result through a 'var' output parameter instead.\n",
                    d->line, d->col, (int)id->length, id->name);
            diagnostic_show_line(d->line, d->col);
            exit(1);
        }
  
        char *rawf = malloc(id->length + 1);
        memcpy(rawf, id->name, id->length);
        rawf[id->length] = '\0';
  
        char *cnamef;
        if (d->kind == DECL_EXTERN_FUNCTION || d->kind == DECL_EXTERN_PROCEDURE) {
            // D-40: an EXTERN may not take a plain dynamic slice. There is no C type for one,
            // so the declaration could not tell the truth about the callee whatever it emitted:
            // it was declared `const Slice_u8*` while the call site passed the decomposed data
            // pointer, and even forwarding a `u8[]` parameter to such an extern warned.
            //
            // `extern` exists to describe functions Lain did not write, and a C function takes a
            // pointer and, separately, a length. `std/` already declares them that way —
            // `libc_printf(fmt *u8, ...)` — so this rejects a form nothing depends on and
            // matches what the standard library had already settled on.
            //
            // A SENTINEL slice (`u8[:0]`) is exempt: it is NUL-terminated, so it genuinely is a
            // C string and is already declared `const char*`.
            for (DeclList *pp = d->as.function_decl.params; pp; pp = pp->next) {
                if (!pp->decl || pp->decl->kind != DECL_VARIABLE) continue;
                Type *pt = pp->decl->as.variable_decl.type;
                bool dyn_slice = pt &&
                    ((pt->kind == TYPE_SLICE && !pt->sentinel_is_string && !pt->sentinel_str &&
                      pt->sentinel_len == 0) ||
                     (pt->kind == TYPE_ARRAY && pt->array_len == -1));
                if (!dyn_slice) continue;
                Id *pn = pp->decl->as.variable_decl.name;
                fprintf(stderr, "[E131] Error Ln %li, Col %li: extern '%.*s' declares parameter "
                    "'%.*s' as a slice, which has no C representation — the declaration cannot "
                    "describe the function being bound. Pass a pointer and a length instead "
                    "(`p *u8, n usize`), as std/ does.\n",
                    (long)(d->line ? d->line : (pp->decl ? pp->decl->line : 0)),
                    (long)(d->line ? d->col  : (pp->decl ? pp->decl->col  : 0)),
                    (int)id->length, id->name,
                    pn ? (int)pn->length : 1, pn ? pn->name : "?");
                diagnostic_show_line(d->line ? d->line : (pp->decl ? pp->decl->line : 0),
                                     d->line ? d->col  : (pp->decl ? pp->decl->col  : 0));
                exit(1);
            }
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
  
        // ★ A FIELD INVARIANT THE COMPILER IGNORES IS AN UNPAID ASSUME. A field refinement whose
        // right-hand side is a LITERAL is enforced at construction and usable in proofs; one that
        // names ANOTHER FIELD — `type B { cap usize, pos usize <= cap }` — parsed, and then did
        // nothing at all: it was not carried as a fact and `B(5, 10)` compiled clean. A reader
        // has every reason to believe the invariant holds, and nothing checks it.
        //
        // Relational field invariants are planned (the `in`-invariant work) and not built. Until
        // they are, say so. Nothing in std/ or tests/ uses the form, so refusing it costs
        // nothing, and refusing what is not checked is the same rule the IR applies when it
        // marks a function `incomplete` rather than passing it silently.
        for (DeclList *f = d->as.struct_decl.fields; f; f = f->next) {
            if (!f->decl || f->decl->kind != DECL_VARIABLE) continue;
            for (ExprList *c = f->decl->as.variable_decl.constraints; c; c = c->next) {
                if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
                Expr *rhs = c->expr->as.binary_expr.right;
                if (!rhs || rhs->kind == EXPR_LITERAL) continue;
                Id *fnm = f->decl->as.variable_decl.name;
                fprintf(stderr,
                    "[E132] Error Ln %li, Col %li: field '%.*s' of struct '%.*s' has a refinement "
                    "that names another field.\n"
                    "       Relational field invariants are not implemented, and this one would be\n"
                    "       silently ignored: it is neither checked when the struct is built nor\n"
                    "       usable as a fact afterwards. Refusing it rather than pretending.\n"
                    "       A refinement against a LITERAL (`%.*s usize <= 4096`) is supported.\n",
                    (long)(c->expr->line ? c->expr->line : (f->decl ? f->decl->line : d->line)),
                    (long)(c->expr->line ? c->expr->col  : (f->decl ? f->decl->col  : d->col)),
                    (int)(fnm ? fnm->length : 0), fnm ? fnm->name : "",
                    (int)id->length, id->name,
                    (int)(fnm ? fnm->length : 0), fnm ? fnm->name : "");
                diagnostic_show_line(c->expr->line ? c->expr->line : (f->decl ? f->decl->line : d->line),
                                     c->expr->line ? c->expr->col  : (f->decl ? f->decl->col  : d->col));
                exit(1);
            }
        }

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
      // D-30: a mutable borrow may not be STORED IN A LOCAL either. E126 rejected it as a
      // struct FIELD; the same construct as a local binding was accepted and emitted C that
      // does not compile — `var r = var a` gives `P r = &(a);`, an "invalid initializer".
      // The scalar case was worse because it was SILENT: `var ref = var d.value` emits
      // `int32_t ref = &(d.value);`, which gcc only WARNS about, storing a truncated address
      // in an int. The one corpus test over it passed solely because it never read the
      // binding.
      //
      // Rejecting makes the restriction uniform, which is the point: the borrow checker is
      // 535 lines and call-site-scoped BECAUSE a reference cannot be stored, so every hole in
      // that rule is load-bearing rather than cosmetic. It is also exactly the axiom of
      // mutable value semantics — references are created at call boundaries and stored
      // "neither in variables nor in object fields" — which Lain had adopted for fields only.
      //
      // This removes no working feature: the construct has never compiled correctly.
      if (rhs->kind == EXPR_MUT) {
          Id *nm = s->as.var_stmt.name;
          fprintf(stderr, "[E126] Error Ln %li, Col %li: '%.*s' binds a mutable borrow (`var`), "
              "which cannot be stored — a reference lives only for the call that creates it. "
              "Bind the value instead, or pass the borrow directly to the function that needs it.\n",
              (long)s->line, (long)s->col,
              nm ? (int)nm->length : 1, nm ? nm->name : "?");
          diagnostic_show_line(s->line, s->col);
          exit(1);
      }
      if (sema_ranges) {
          // Range analysis moved to typecheck phase
      }
      if (!ty) {
          ty = rhs->type;           // infer from initializer
          // D-38 tier 1. A CALL returning `var T` yields an ADDRESS. Stripping its mode to
          // SHARED declared the local at the VALUE type while the initialiser stayed a pointer
          // — `int32_t ref = <int32_t*>`. gcc calls that a warning, not an error, so the
          // broken-C gate (which compiles with -w) never saw it, and seven corpus programs
          // shipped a truncated address in an integer.
          //
          // REJECTING it was tried first, on the reasoning that Hylo's LocalBindings.md says an
          // `inout` binding "never has storage", so `var` (which always has storage) cannot name
          // a borrow. That is right about the SPELLING and wrong as a fix: it broke 16 tests, and
          // 9 of them are `_fail` tests that use this construct to SET UP a conflict. Rejecting
          // it removes the only way Lain can express a loan that outlives a statement, and makes
          // the borrow checker's entire conflict-detection capability unreachable. The construct
          // is not an incidental spelling; it is the language's `inout` binding wearing `var`'s
          // keyword because there is no other.
          //
          // So: keep the binding and give it the type it actually has. A `var T` initialiser
          // keeps MODE_MUTABLE, which the backend already emits as `T*` — it does so for every
          // `var` parameter. What a future language revision should add is the distinct binding
          // form (plan §10.4); until then this binding means "name this borrow" and is lowered
          // as one, which is what `borrow.h` phase 3 already assumes.
          // Strip MODE_MUTABLE: `var x = var_param` gives value type, not reference — reading a
          // `var` PARAMETER yields the pointee, so binding it copies. That is a value binding and
          // stays correct.
          // Exception: TYPE_POINTER with MODE_MUTABLE is a mutable thin pointer
          // (from `&arr[k]`) — preserve mutability so it emits without const.
          if (ty && ty->mode == MODE_MUTABLE && ty->kind != TYPE_POINTER
              && rhs->kind != EXPR_CALL) {
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

    if (is_reserved_type_name(raw)) {
        fprintf(stderr, "[E013] Error Ln %li, Col %li: '%s' is a builtin type name and "
                "cannot be used as a variable name.\n", s->line, s->col, raw);
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
        // ── the loop variable takes the type of its BOUND ────────────────────────────────
        // `for i in 0..out.len` was giving `i` an i32 unconditionally. A length is a usize,
        // so the guard compares an i32 against a usize, nothing bounds the length below
        // INT32_MAX, and the increment `i + 1` can genuinely leave i32 — which the overflow
        // check then refuses, CORRECTLY, on a loop the programmer wrote in the obvious way.
        //
        // The A1 precision survey put this shape at 12% of every unproven obligation in the
        // corpus, the single cheapest item measured. The bound already carries the right
        // type; the loop variable just was not asking for it.
        //
        // The START is only consulted when the END has no type, and a literal `0` start is
        // ignored on purpose: `0..out.len` should follow `out.len`, not the literal.
        Type *bt = NULL;
        Expr *end = it->as.range_expr.end, *st = it->as.range_expr.start;
        // inference on the range itself does not necessarily type its endpoints
        if (end) { sema_resolve_expr(end); sema_infer_expr(end); }
        if (st)  { sema_resolve_expr(st);  sema_infer_expr(st);  }
        if (end && end->type && end->type->kind == TYPE_SIMPLE) bt = end->type;
        else if (st && st->type && st->type->kind == TYPE_SIMPLE && st->kind != EXPR_LITERAL)
            bt = st->type;
        val_ty = bt ? bt : get_builtin_i32_type();
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
      if (is_reserved_type_name(raw_i)) {
          fprintf(stderr, "[E013] Error Ln %li, Col %li: '%s' is a builtin type name and "
                  "cannot be used as a loop variable.\n", s->line, s->col, raw_i);
          diagnostic_show_line(s->line, s->col); exit(1);
      }
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
      if (is_reserved_type_name(raw_c)) {
          fprintf(stderr, "[E013] Error Ln %li, Col %li: '%s' is a builtin type name and "
                  "cannot be used as a loop variable.\n", s->line, s->col, raw_c);
          diagnostic_show_line(s->line, s->col); exit(1);
      }
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
        // ★ A `var` SLICE parameter cannot be reassigned AS A WHOLE, because neither backend
        // can express it and each got it wrong in a different way. A slice parameter is
        // decomposed at the ABI boundary into `(size_t __len_s, T *s)` — which is what earns
        // the `restrict` and `access` annotations — so there is no single lvalue to assign to:
        // the old emitter produced `s = (Slice_i32){...}` against an `int32_t *` and the C
        // compiler REFUSED it, while the new one passes the slice by value and silently drops
        // the write (a `shrink` that leaves len at 8). Every other `var` parameter shape
        // propagates correctly — scalar, whole struct, struct field — so this is the one place
        // the rule quietly fails, and it failed with no diagnostic at all.
        //
        // Writing THROUGH the slice (`s[i] = v`) is unaffected: that is an element store, not
        // a rebind, and it is the operation the decomposed ABI exists to make fast.
        Type *vpt = sym->decl->as.variable_decl.type;
        while (vpt && vpt->kind == TYPE_COMPTIME) vpt = vpt->element_type;
        if (vpt && ((vpt->kind == TYPE_ARRAY && vpt->array_len < 0) || vpt->kind == TYPE_SLICE)) {
          fprintf(stderr, "[E009] Error Ln %li, Col %li: cannot reassign the `var` slice "
                  "parameter '%s' as a whole.\n"
                  "       A slice parameter is passed as a length and a pointer, so there is no "
                  "single value to\n"
                  "       assign back to; the caller would not see the new length.\n"
                  "       Options to resolve:\n"
                  "         (a) Write through it instead: `%s[i] = v` modifies the caller's "
                  "elements.\n"
                  "         (b) Return the new slice and let the caller rebind it.\n"
                  "         (c) Take the length as its own `var usize` parameter if the length "
                  "is what changes.\n",
                  s->line, s->col, raw, raw);
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
            if (!is_param && !g_suppress_ownership) {   // seam: deferred to the new IR borrow pass
                fprintf(stderr, "[E010] Error Ln %li, Col %li: Returning a mutable reference ('var') to a local variable is forbidden (dangling pointer)\n", s->line, s->col);
                diagnostic_show_line(s->line, s->col);
                exit(1);
            }
        }
    }
    break;

  case STMT_MATCH: {
    sema_resolve_expr(s->as.match_stmt.value);
    // The scrutinee's TYPE is needed twice below (to type the arms' payload bindings, and
    // for exhaustiveness), and resolution alone produces one only for an identifier.
    if (s->as.match_stmt.value && !s->as.match_stmt.value->type)
      sema_infer_expr(s->as.match_stmt.value);
    Decl *marm_enum = s->as.match_stmt.value
                    ? find_enum_decl(s->as.match_stmt.value->type) : NULL;
    for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
      sema_push_scope();
      // A variant pattern BINDS its payload: `Pt(p)` introduces `p` with the variant field's
      // type. Those bindings were never declared, so they were UNTYPED — which happens to
      // work for a scalar (`A(v): v + 1` needs no type) and fails the moment anything asks
      // for one: `Pt(p): p.x` died with E102 "'p' is not a value".
      if (marm_enum) for (ExprList *p = c->patterns; p; p = p->next) {
        Expr *pe = p->expr;
        if (!pe || pe->kind != EXPR_CALL || !pe->as.call_expr.callee) continue;
        Expr *cal = pe->as.call_expr.callee;
        Id *vn = cal->kind==EXPR_MEMBER     ? cal->as.member_expr.member
               : cal->kind==EXPR_IDENTIFIER ? cal->as.identifier_expr.id : NULL;
        if (!vn) continue;
        for (Variant *v = marm_enum->as.enum_decl.variants; v; v = v->next) {
          if (!v->name || v->name->length != vn->length ||
              strncmp(v->name->name, vn->name, (size_t)vn->length) != 0) continue;
          ExprList *a = pe->as.call_expr.args; DeclList *fl = v->fields;
          for (; a && fl; a = a->next, fl = fl->next) {
            if (!a->expr || a->expr->kind != EXPR_IDENTIFIER) continue;
            if (!fl->decl || fl->decl->kind != DECL_VARIABLE) continue;
            Id *bn = a->expr->as.identifier_expr.id;
            if (!bn) continue;
            char raw[256]; int bl = (int)bn->length; if (bl > 255) bl = 255;
            memcpy(raw, bn->name, (size_t)bl); raw[bl] = 0;
            Type *fty = fl->decl->as.variable_decl.type;
            sema_insert_local(raw, raw, fty, fl->decl, false);
            a->expr->type = fty;                       // the pattern occurrence itself
          }
          break;
        }
      }
      for (ExprList *p = c->patterns; p; p = p->next) {
        sema_resolve_expr(p->expr);
      }
      for (StmtList *b = c->body; b; b = b->next) {
        sema_resolve_stmt(b->stmt);
      }
      sema_pop_scope();
    }
    // Check exhaustiveness after resolving all cases (the scrutinee was typed above —
    // without it `case o.tag` on a struct FIELD reported a fully-covered match as
    // non-exhaustive, because a member expression has no type until inference runs).
    if (!sema_check_match_exhaustive(s)) {
      sema_report_nonexhaustive_match(s);
      exit(1);
    }
    break;
  }
  
  case STMT_WHILE: {
    // Purity: while loops without a termination measure are banned in pure functions,
    // UNLESS the condition consists entirely of pointer-in-arr guards (p in arr where
    // p has TYPE_POINTER): the walk phase will auto-synthesize the measure from the
    // monotone pointer decrement pattern.
    // ── E.4 PREREQUISITE: the loop ban must read the ROW, not the keyword ────────────────
    // `func` is pure and total by default, and `effects diverge` is how a function says it may
    // not terminate (plan 7B). Keying this on `kind == DECL_FUNCTION` alone meant that once
    // `proc` starts disappearing, every migrated loop became an error with a message telling the
    // programmer to "use 'proc'" — advice for a keyword that is going away.
    //
    // The sovereign engine raises the real obligation per loop header (analysis/vra.h) and reads
    // `may_diverge`; this front-end check is the older, coarser one and now agrees with it.
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION
        && !current_function_decl->as.function_decl.diverges
        && !(current_function_decl->as.function_decl.effects_declared
             && (current_function_decl->as.function_decl.effects_bound & EFFECT_DIVERGE))) {
        if (!s->as.while_stmt.measure) {
            // Structural scan: an `expr in expr` guard OR a relational comparison
            // (`i < n`, `i >= k`, …) in the condition → defer to the walk phase,
            // which auto-synthesizes and verifies a measure (`n - i` etc.). Only a
            // condition with no such shape (e.g. `while true`, `while flag`) is
            // genuinely un-inferable and rejected early.
            bool deferrable = false;
            {
                Expr *stk[16]; int top = 0; stk[top++] = s->as.while_stmt.cond;
                while (top > 0) {
                    Expr *e = stk[--top];
                    if (!e || e->kind != EXPR_BINARY) continue;
                    TokenKind op = e->as.binary_expr.op;
                    if (op == TOKEN_KEYWORD_IN ||
                        op == TOKEN_ANGLE_BRACKET_LEFT || op == TOKEN_ANGLE_BRACKET_LEFT_EQUAL ||
                        op == TOKEN_ANGLE_BRACKET_RIGHT || op == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL) {
                        deferrable = true; break;
                    }
                    if (op == TOKEN_KEYWORD_AND && top < 14) {
                        stk[top++] = e->as.binary_expr.left;
                        stk[top++] = e->as.binary_expr.right;
                    }
                }
            }
            if (!deferrable) {
                fprintf(stderr, "[E011] Error Ln %li, Col %li: 'while' loops without a termination measure "
                        "are not allowed in pure function '%.*s'. "
                        "Add 'decreasing <measure>' or use 'proc'.\n",
                        s->line, s->col,
                        (int)current_function_decl->as.function_decl.name->length,
                        current_function_decl->as.function_decl.name->name);
                diagnostic_show_line(s->line, s->col);
                exit(1);
            }
            // deferrable: the walk phase auto-infers + verifies the measure, or emits E011
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

 case STMT_ASSERT: {
    // `assume` hands the engine a fact NOTHING CHECKS, so it is only sound where the
    // programmer has already taken responsibility. That is exactly what `unsafe` means here,
    // and the restriction is not decoration: an unchecked annotation trusted by the prover is
    // the shape of defect D-4, where an `in` invariant became `__builtin_unreachable()` with
    // nothing verifying it. `assert` is always allowed — it is an obligation, not a licence.
    if (s->as.assert_stmt.is_assume && !sema_in_unsafe_block) {
        fprintf(stderr, "[E129] Error Ln %li, Col %li: `assume` states a fact the compiler "
                "does not check, so it is only allowed inside an `unsafe` block. Use "
                "`assert` if you want the fact PROVEN instead.\n", s->line, s->col);
        diagnostic_show_line(s->line, s->col);
        exit(1);
    }
    sema_resolve_expr(s->as.assert_stmt.cond);
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

    // ── Purity: a `func` may not call a `proc` UNLESS it declared the effect (B.1) ────────
    //
    // ★ THE DECLARED ROW HAD TO START MEANING SOMETHING. This check was purely syntactic —
    // DECL_FUNCTION calling DECL_PROCEDURE, refused regardless of any `effects` clause — so
    // `func f() i32 effects io` was rejected identically to `func f() i32`. The clause could
    // therefore never be satisfied by anything but the empty row: it narrowed nothing and
    // widened nothing, which made `effects` on a `func` ANOTHER ASSERTION OF NOTHING, the same
    // defect as `effects write` one level up.
    //
    // A declared row is an upper BOUND that the programmer states and the compiler checks
    // (sema's declared-vs-inferred comparison does the checking). Stating `io` means "this
    // performs IO" — so calling a proc is precisely what was declared, not a violation of it.
    // Without the clause the bound is ∅ and the refusal stands, which is what keeps `func`'s
    // purity guarantee the default rather than an opt-in.
    //
    // This is the prerequisite for deleting `proc` (DECIDE-F): one introducer whose bound is ∅
    // by default and `effects …` to widen it. Until the clause was load-bearing, "delete
    // `proc`" had nothing to delete it in favour of.
    // ★ THE KEYWORD-KEYED PURITY CHECK IS GONE (E.5/E.6). It refused a call whose CALLEE
    // was spelled `proc`, regardless of what the callee actually does, so `proc hello() { }` —
    // row ∅, a function that does nothing — could not be called from a pure `func`. The row
    // subsumes it and is strictly more precise: an effect is refused when it is an effect, not
    // when it is introduced by a particular word. Three further reasons it had to go:
    //
    //   · it was the last place a KEYWORD granted or refused an effect, which is what law L3
    //     refuses and what E.4 removed everywhere else;
    //   · it ran during RESOLVE, before the effect fixpoint, so it won the race against the
    //     row check and its accident became a documented precedence rule (plan 7B.10) that
    //     no longer described the language;
    //   · it could only see a DIRECT call to a `proc`, never a `func` that does IO through
    //     three layers — exactly the case the transitive row catches.
    //
    // What it had that the row did not is the CALL SITE. That is now carried by the row check
    // instead (`eff_site_*`), so the position is kept and the precision is gained.
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

  case EXPR_TRY:
    sema_resolve_expr(e->as.try_expr.operand);
    break;

  case EXPR_ELSE:
    sema_resolve_expr(e->as.else_expr.operand);
    sema_resolve_expr(e->as.else_expr.arm);
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
        if (is_reserved_type_name(raw)) {
            fprintf(stderr, "[E013] Error Ln %li, Col %li: '%s' is a builtin type name and "
                    "cannot be used as a comprehension variable.\n", e->line, e->col, raw);
            exit(1);
        }
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
        case BUILTIN_CTZ:
        case BUILTIN_CLZ:
        case BUILTIN_POPCOUNT:
        case BUILTIN_MOVEMASK:
            // Resolve the inner argument, keep node as-is
            if (e->as.builtin_expr.arg)
                sema_resolve_expr(e->as.builtin_expr.arg);
            break;
        case BUILTIN_LOAD:
        case BUILTIN_SPLAT:
        case BUILTIN_STORE:
        case BUILTIN_SHUFFLE:
            if (e->as.builtin_expr.arg)  sema_resolve_expr(e->as.builtin_expr.arg);
            if (e->as.builtin_expr.arg2) sema_resolve_expr(e->as.builtin_expr.arg2);
            if (e->as.builtin_expr.arg3) sema_resolve_expr(e->as.builtin_expr.arg3);
            if (e->as.builtin_expr.vec_type)
                e->as.builtin_expr.vec_type = mono_resolve_type_apps(e->as.builtin_expr.vec_type);
            break;
    }
    break;
  }

  default:
    break;
  }
}

#endif /* SEMA_RESOLVE_H */
