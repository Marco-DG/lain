#ifndef EMIT_DECL_H
#define EMIT_DECL_H

#include "../emit.h"

// Emit one top-level declaration
void emit_decl(Decl *decl, int depth);
static void emit_forward_decl(Decl *decl, int depth);
// Emit a list of declarations (the whole program)
void emit_decl_list(DeclList *decls, int depth);

static void emit_param_type(Type *t, bool with_restrict); // Forward for use in forward decl

// D-Niche: compute the C backing type for a niche-optimized enum (multi ->
// primary field; pure-empty -> int32_t; bool payload -> uint8_t; else the
// payload field's type). Shared by the forward-decl pass (emit.h) and the
// definition pass below so the scalar typedef and its constructors agree.
static void emit_niche_backing_type(Decl *decl, NicheLayout *nl, char *backing, size_t n) {
    bool is_multi = (nl->primary_variant != NULL && nl->secondary_variant != NULL);
    Variant *primary = is_multi ? nl->primary_variant
                                : enum_payload_variant(&decl->as.enum_decl);
    if (!primary) {
        snprintf(backing, n, "int32_t");
    } else {
        Type *ft = primary->fields->decl->as.variable_decl.type;
        if (!is_multi && nl->pool.kind == POOL_BOOL) snprintf(backing, n, "uint8_t");
        else c_name_for_type(ft, backing, n);
    }
}

// Q-019 [pure]: returns true if any parameter of decl is a var (MODE_MUTABLE) borrow.
// A DECL_FUNCTION with no var params has no caller-visible side effects and qualifies
// for __attribute__((pure)): Lain already guarantees no mutable global access and
// no proc calls, so the only possible side-effect channel is var parameters.
static bool func_has_var_param(Decl *decl) {
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next) {
        if (!p->decl) continue;
        Type *pt = (p->decl->kind == DECL_VARIABLE)   ? p->decl->as.variable_decl.type
                 : (p->decl->kind == DECL_DESTRUCT)    ? p->decl->as.destruct_decl.type
                 : NULL;
        if (pt && pt->mode == MODE_MUTABLE) return true;
    }
    return false;
}

// A function that takes a function-pointer parameter may CALL it, and an
// indirect call is an unanalysed effect (a `*proc` target can diverge or
// mutate). Such a function must therefore NOT be marked const/pure, or gcc
// could CSE/elide calls and drop real effects. (The Phase-3 effect system will
// refine this to allow `pure` when the fn-ptr param is a side-effect-free
// `*func`.)
static bool func_has_fnptr_param(Decl *decl) {
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next) {
        if (!p->decl) continue;
        Type *pt = (p->decl->kind == DECL_VARIABLE)   ? p->decl->as.variable_decl.type
                 : (p->decl->kind == DECL_DESTRUCT)    ? p->decl->as.destruct_decl.type
                 : NULL;
        if (pt && pt->kind == TYPE_FUNC) return true;
    }
    return false;
}

// Q-019c [const]: returns true if every parameter of decl is passed by value in C
// (primitive shared borrows or owned values — no pointer parameters at all).
// When combined with !func_has_var_param, the function qualifies for the stronger
// __attribute__((const)): no pointer params → no indirect memory reads → return
// value depends only on by-value arguments regardless of memory state.
static bool func_all_params_by_value(Decl *decl) {  // also used by func_has_ptr_param
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next) {
        if (!p->decl) continue;
        Type *pt = (p->decl->kind == DECL_VARIABLE) ? p->decl->as.variable_decl.type
                 : (p->decl->kind == DECL_DESTRUCT)  ? p->decl->as.destruct_decl.type
                 : NULL;
        if (!pt) continue;
        if (pt->kind == TYPE_COMPTIME) continue;
        if (pt->kind == TYPE_ARRAY) return false;          // decomposed to ptr
        // A pointer or slice parameter is passed by value in C, but the function
        // READS memory THROUGH it. gcc's __attribute__((const)) means "reads no
        // memory at all" — emitting it here lets -O2 CSE/hoist those loads across
        // a store to the pointee and silently miscompile. is_primitive_type()
        // returns true for TYPE_POINTER/TYPE_SLICE, so they must be excluded
        // explicitly; they then fall back to the sound `pure` (may read memory).
        if (pt->kind == TYPE_POINTER) return false;        // reads through *p
        if (pt->kind == TYPE_SLICE) return false;          // reads through .data
        if (pt->mode == MODE_MUTABLE) return false;        // T * restrict
        if (pt->mode == MODE_SHARED && !is_primitive_type(pt)) return false; // const T*
    }
    return true;
}

// Q-020 [nonnull]: returns true if any parameter becomes a pointer in the C ABI.
// The Lain borrow checker guarantees every borrow (var T or shared T) is a valid,
// non-null reference at the call site — never a null or dangling pointer.
// This mirrors the exact conditions in func_all_params_by_value (inverted).
static bool func_has_ptr_param(Decl *decl) {
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next) {
        if (!p->decl) continue;
        Type *pt = (p->decl->kind == DECL_DESTRUCT)  ? p->decl->as.destruct_decl.type
                 : (p->decl->kind == DECL_VARIABLE)  ? p->decl->as.variable_decl.type
                 : NULL;
        if (!pt) continue;
        if (pt->kind == TYPE_COMPTIME) continue;
        if (pt->kind == TYPE_ARRAY) return true;
        if (pt->mode == MODE_MUTABLE) return true;
        if (pt->mode == MODE_SHARED && !is_primitive_type(pt)) return true;
    }
    return false;
}

// Q-021 [returns_nonnull]: returns true if the function's return type becomes
// a non-null pointer in the C ABI.  Only borrow returns qualify: MODE_MUTABLE
// (var T return → T*) and MODE_SHARED on non-primitives (shared struct → const T*).
// Raw TYPE_POINTER returns are excluded — those may legitimately be null.
static bool func_returns_nonnull_ptr(Decl *decl) {
    if (!decl) return false;
    if (decl->kind != DECL_FUNCTION && decl->kind != DECL_PROCEDURE) return false;
    Type *rt = decl->as.function_decl.return_type;
    if (!rt) return false;
    if (rt->kind == TYPE_COMPTIME) return false;
    if (rt->kind == TYPE_POINTER) return false;  // raw pointer: may be null
    // Slice/dynamic-array returns are emitted as a Slice_<T> STRUCT (by value),
    // not a C pointer — returns_nonnull on them is invalid ("attribute on a
    // function not returning a pointer"). Exclude all array/slice returns
    // regardless of mode.
    if (rt->kind == TYPE_ARRAY) return false;
    if (rt->mode == MODE_MUTABLE) return true;
    // MODE_SHARED non-primitive: only pointer types return as C pointers (not structs)
    if (rt->mode == MODE_SHARED && rt->kind == TYPE_POINTER) return true;
    return false;
}

// ---- parameter write detection ---------------------------------------------
// A dynamic-array/pointer parameter written through in the body (out[i]=…, *p=…,
// p.f=…) cannot be emitted `const`, and its presence makes the function impure
// (a caller-visible side-effect channel) — even for a `func` with a sized output
// parameter such as `out *i32[a.len]`. Without this, such a param emitted as
// `const T * restrict` produced C that gcc rejects ("assignment of read-only
// location"), and `__attribute__((pure))` on it was semantically wrong.
static Id *emit_lvalue_root_id(Expr *e) {
    while (e) {
        switch (e->kind) {
            case EXPR_IDENTIFIER: return e->as.identifier_expr.id;
            case EXPR_INDEX:      e = e->as.index_expr.target;  break;
            case EXPR_MEMBER:     e = e->as.member_expr.target; break;
            case EXPR_DEREF:      e = e->as.deref_expr.expr;    break;
            default:              return NULL;
        }
    }
    return NULL;
}
static bool emit_id_eq(Id *a, Id *b) {
    return a && b && a->length == b->length &&
           memcmp(a->name, b->name, (size_t)a->length) == 0;
}
static bool emit_stmtlist_writes_id(StmtList *body, Id *pname);
static bool emit_stmt_writes_id(Stmt *s, Id *pname) {
    if (!s) return false;
    switch (s->kind) {
        case STMT_ASSIGN:
            return emit_id_eq(emit_lvalue_root_id(s->as.assign_stmt.target), pname);
        case STMT_IF:
            return emit_stmtlist_writes_id(s->as.if_stmt.then_body, pname) ||
                   emit_stmtlist_writes_id(s->as.if_stmt.else_branch, pname);
        case STMT_FOR:
            return emit_stmtlist_writes_id(s->as.for_stmt.body, pname);
        case STMT_WHILE:
            return emit_stmtlist_writes_id(s->as.while_stmt.body, pname);
        case STMT_UNSAFE:
            return emit_stmtlist_writes_id(s->as.unsafe_stmt.body, pname);
        case STMT_DEFER:
            return emit_stmt_writes_id(s->as.defer_stmt.stmt, pname);
        case STMT_COMPTIME_IF:
            return emit_stmtlist_writes_id(s->as.comptime_if_stmt.then_body, pname) ||
                   emit_stmtlist_writes_id(s->as.comptime_if_stmt.else_branch, pname);
        case STMT_MATCH:
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next)
                if (emit_stmtlist_writes_id(c->body, pname)) return true;
            return false;
        default:
            return false;
    }
}
static bool emit_stmtlist_writes_id(StmtList *body, Id *pname) {
    for (; body; body = body->next)
        if (emit_stmt_writes_id(body->stmt, pname)) return true;
    return false;
}
// True if `func`/`proc` `decl` writes through the given parameter decl.
static bool emit_param_is_written(Decl *decl, Decl *param) {
    if (!decl || !param || param->kind != DECL_VARIABLE) return false;
    if (decl->kind != DECL_FUNCTION && decl->kind != DECL_PROCEDURE) return false;
    Id *pn = param->as.variable_decl.name;
    if (!pn) return false;
    return emit_stmtlist_writes_id(decl->as.function_decl.body, pn);
}
// True if the function writes through ANY parameter (=> not pure/const).
static bool func_writes_through_param(Decl *decl) {
    if (decl->kind != DECL_FUNCTION && decl->kind != DECL_PROCEDURE) return false;
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next)
        if (p->decl && emit_param_is_written(decl, p->decl)) return true;
    return false;
}

// O-001 [access]: for each TYPE_ARRAY param whose size_expr is a simple
// EXPR_IDENTIFIER referencing another param, emit
//   __attribute__((access(read_only|read_write, ptr_idx, size_idx)))
// This tells GCC the exact buffer bound for each sized-array parameter,
// enabling better alias analysis and caller-side static bounds checking.
// Only emitted when size_expr is a bare identifier (not a complex expression
// like m+n) so the mapping to a parameter index is unambiguous.
static void emit_access_attributes(Decl *decl) {
    if (!decl) return;
    DeclList *params = decl->as.function_decl.params;
    // Build a flat array of (name, index) for lookup.
    // NOTE: c_name_for_id returns a static buffer — must copy each string.
    #define MAX_PARAMS 32
    char pname_bufs[MAX_PARAMS][256]; const char *pnames[MAX_PARAMS]; int nparams = 0;
    for (DeclList *p = params; p && nparams < MAX_PARAMS; p = p->next) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) { pnames[nparams++] = NULL; continue; }
        const char *cn = c_name_for_id(p->decl->as.variable_decl.name);
        strncpy(pname_bufs[nparams], cn, 255); pname_bufs[nparams][255] = '\0';
        pnames[nparams] = pname_bufs[nparams];
        nparams++;
    }
    int pidx = 0;
    for (DeclList *p = params; p; p = p->next, pidx++) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Type *pt = p->decl->as.variable_decl.type;
        if (!pt || pt->kind != TYPE_ARRAY) continue;          // only array params
        if (!pt->size_expr) continue;                          // unsized: skip
        if (pt->size_relop != TOKEN_EQUAL_EQUAL) continue;    // constraint bound (i32[>= n]): length is __len_x at runtime, not n
        if (pt->size_expr->kind != EXPR_IDENTIFIER) continue; // complex expr: skip
        // Find the size param by matching the mangled name
        Id *sid = pt->size_expr->as.identifier_expr.id;
        int sidx = -1;
        for (int i = 0; i < nparams; i++) {
            if (!pnames[i]) continue;
            if ((size_t)sid->length == strlen(pnames[i]) &&
                strncmp(sid->name, pnames[i], sid->length) == 0) {
                sidx = i; break;
            }
        }
        if (sidx < 0) {
            // size param name not found by C name — try original Lain name
            for (DeclList *q = params; q; q = q->next) {
                if (!q->decl || q->decl->kind != DECL_VARIABLE) continue;
                Id *qn = q->decl->as.variable_decl.name;
                if (qn->length == sid->length &&
                    strncmp(qn->name, sid->name, sid->length) == 0) {
                    // found — compute its 0-based index
                    int qi = 0;
                    for (DeclList *r = params; r && r != q; r = r->next) qi++;
                    sidx = qi; break;
                }
            }
        }
        if (sidx < 0) continue; // can't find size param
        // A written output param (sized `out *T[n]`) is read_write even though its
        // mode is SHARED — emitting read_only here would license the optimizer to
        // assume it is never stored to (miscompilation), so honor actual writes.
        const char *mode = (pt->mode == MODE_MUTABLE || emit_param_is_written(decl, p->decl))
                           ? "read_write" : "read_only";
        // GCC access indices are 1-based
        EMIT("__attribute__((access(%s, %d, %d))) ", mode, pidx + 1, sidx + 1);
    }
    #undef MAX_PARAMS
}

static void emit_forward_decl(Decl *decl, int depth) {
    if (!decl) return;
    if (decl->kind == DECL_FUNCTION || decl->kind == DECL_PROCEDURE) {
        emit_indent(depth);
        // [private] → internal linkage. The forward declaration MUST carry the
        // same `static` as the definition, or gcc errors "static declaration
        // follows non-static declaration".
        {
            const char *_fn = decl->as.function_decl.name->name;
            size_t _fl = decl->as.function_decl.name->length;
            bool _is_main = (_fl == 4 && strncmp(_fn, "main", 4) == 0);
            if (decl->is_private && !_is_main) EMIT("static ");
        }
        // @cold / @hot: programmer-declared frequency hints.
        if (decl->as.function_decl.is_cold) EMIT("__attribute__((cold)) ");
        if (decl->as.function_decl.is_hot)  EMIT("__attribute__((hot)) ");
        // @allocator: return pointer doesn't alias any existing pointer.
        if (decl->as.function_decl.is_allocator)
            EMIT("__attribute__((malloc, returns_nonnull)) ");
        // @noreturn: function never returns (exit, panic, infinite loop).
        if (decl->as.function_decl.is_noreturn)
            EMIT("__attribute__((noreturn)) ");
        // O-001 [access]: sized-array params → buffer bound annotation for GCC.
        emit_access_attributes(decl);
        // Q-019 [pure/const]: forward-declare func without var params as
        // __attribute__((const)) when all params are by-value, otherwise
        // __attribute__((pure)). Both enable LICM/CSE; const is stronger.
        // A func that writes through a pointer param (sized output param) has a
        // side effect and qualifies for neither.
        if (decl->kind == DECL_FUNCTION && decl->as.function_decl.return_type &&
            !func_has_var_param(decl) && !func_writes_through_param(decl) &&
            !func_has_fnptr_param(decl)) {
            if (func_all_params_by_value(decl))
                EMIT("__attribute__((const)) ");
            else
                EMIT("__attribute__((pure)) ");
        }
        // Q-020 [nonnull]: every borrow (var T or shared T) is provably non-null;
        // emit nonnull with no args to cover all pointer parameters at once.
        if (func_has_ptr_param(decl))
            EMIT("__attribute__((nonnull)) ");
        // Q-021 [returns_nonnull]: borrow return types are provably non-null pointers.
        if (func_returns_nonnull_ptr(decl))
            EMIT("__attribute__((returns_nonnull)) ");
        if (decl->as.function_decl.return_type) {
            emit_type(decl->as.function_decl.return_type);
        } else {
            const char *_fn = decl->as.function_decl.name->name;
            size_t _fl = decl->as.function_decl.name->length;
            if (_fl == 4 && strncmp(_fn, "main", 4) == 0)
                EMIT("int32_t"); // C99: main must return int
            else
                EMIT("void");
        }

        const char *id_name = decl->as.function_decl.name->name;
        size_t id_len = decl->as.function_decl.name->length;
        if (id_len == 4 && strncmp(id_name, "main", 4) == 0) {
            EMIT(" main(");
        } else {
            EMIT(" %s(", c_name_for_id(decl->as.function_decl.name));
        }

        DeclList* param = decl->as.function_decl.params;
        if (param) {
            int first = 1;
            while (param) {
                if (param->decl->kind == DECL_VARIABLE && param->decl->as.variable_decl.type && param->decl->as.variable_decl.type->kind == TYPE_COMPTIME) {
                    param = param->next;
                    continue;
                }
                if (!first) {
                    EMIT(", ");
                }
                if (param->decl->kind == DECL_DESTRUCT) {
                    emit_param_type(param->decl->as.destruct_decl.type, true);
                } else {
                    Type *pt = param->decl->as.variable_decl.type;
                    Id   *pn = param->decl->as.variable_decl.name;
                    if (pt && pt->kind == TYPE_ARRAY && pt->array_len == -1) {
                        // Fase 7: decompose dynamic array param to size_t? + T *
                        char elem_buf[256];
                        c_name_for_type(pt->element_type, elem_buf, sizeof elem_buf);
                        if (dynarray_param_has_runtime_len(pt))
                            EMIT("size_t __len_%.*s, ", (int)pn->length, pn->name);
                        if (pt->mode == MODE_MUTABLE || emit_param_is_written(decl, param->decl))
                            EMIT("%s * restrict", elem_buf);
                        else
                            EMIT("const %s * restrict", elem_buf);
                    } else {
                        emit_param_type(pt, true);
                    }
                }
                first = 0;
                param = param->next;
            }
        } else {
            EMIT("void");
        }
        EMIT(");\n");
    }
}

static void emit_param_type(Type *t, bool with_restrict) {
    if (!t) return;

    // Function-pointer parameter → abstract C declarator `R (*)(P..)`.
    // (The definition site injects the parameter name inside the parens; a
    // prototype uses the abstract form emitted here.)
    if (t->kind == TYPE_FUNC) {
        (void)with_restrict;
        char fb[768];
        c_name_for_fnptr(t, "", fb, sizeof fb);
        EMIT("%s", fb);
        return;
    }

    // Get the base type name without ownership decorations,
    // BUT for Pointers, the mode dictates the C type (const vs non-const),
    // so we must preserve it to generate the correct base "value type".
    OwnershipMode original_mode = t->mode;

    // Get the base type name without ownership decoration WITHOUT mutating the
    // (possibly shared) type node: for non-pointers use a stack-local shallow
    // copy with a neutral mode. Type nodes must stay immutable so they can be
    // interned (P2/S1).
    char base_name[256];
    if (t->kind != TYPE_POINTER) {
        Type tmp = *t;
        tmp.mode = MODE_SHARED;
        c_name_for_type(&tmp, base_name, sizeof(base_name));
    } else {
        c_name_for_type(t, base_name, sizeof(base_name));
    }

    // Now emit the correct C type based on ownership mode
    if (original_mode == MODE_OWNED) {
        // mov T -> pass by value (T)
        EMIT("%s", base_name);
    } else if (original_mode == MODE_MUTABLE) {
        // mut T -> pass as mutable pointer.
        // with_restrict=true for Lain functions: the borrow checker has
        // already proven at every call site that no two mut parameters alias,
        // so `restrict` is a sound annotation and enables SIMD vectorization.
        if (with_restrict) {
            EMIT("%s * restrict", base_name);
        } else {
            EMIT("%s *", base_name);
        }
    } else {
        // Shared Reference (MODE_SHARED)
        if (is_primitive_type(t)) {
            EMIT("%s", base_name);  // Pass by value for primitives
        } else {
            EMIT("const %s*", base_name);  // Pass as const pointer for structs
        }
    }
}

void emit_decl(Decl* decl, int depth) {
    if (!decl) return;
    switch (decl->kind) {
        case DECL_VARIABLE:
            // Top-level constants are emitted earlier (as `static const …`) in the
            // forward pass so they precede every function; nothing to do here.
            break;

        case DECL_EXTERN_PROCEDURE:
        case DECL_EXTERN_FUNCTION: {
             emit_indent(depth);
             EMIT("extern ");
             const char *fname = c_name_for_id(decl->as.function_decl.name);
             if (strcmp(fname, "fgets") == 0) {
                 EMIT("char *");
             } else if (decl->as.function_decl.return_type) {
                 emit_type(decl->as.function_decl.return_type);
             } else {
                 EMIT("void");
             }
             EMIT(" %s(", fname);
            
             DeclList* param = decl->as.function_decl.params;
             if (param) {
                 int first = 1;
                 while (param) {
                     if (!first) EMIT(", ");
                     if (param->decl->kind == DECL_DESTRUCT) {
                          emit_param_type(param->decl->as.destruct_decl.type, false);
                          EMIT(" _destruct_param_");
                     } else {
                          Type *pt = param->decl->as.variable_decl.type;
                          const char *fname = c_name_for_id(decl->as.function_decl.name);
                          // C-interop: a null-terminated `u8[:0]` (a C string) at an
                          // extern boundary is a THIN `const char*`/`char*`, not the
                          // fat `Slice_u8_0` struct. The `*u8`/`*char` hack below only
                          // matches TYPE_SIMPLE elements, so a sentinel slice `u8[:0]`
                          // (or `*u8[:0]`) slipped through and emitted the fat slice,
                          // conflicting with C's `printf(const char*, …)`.
                          Type *ptc = (pt->kind == TYPE_POINTER && pt->element_type)
                                      ? pt->element_type : pt;
                          bool is_cstr = ptc->kind == TYPE_SLICE && ptc->sentinel_str &&
                                         ptc->element_type &&
                                         ptc->element_type->kind == TYPE_SIMPLE &&
                                         ptc->element_type->base_type &&
                                         ptc->element_type->base_type->length == 2 &&
                                         strncmp(ptc->element_type->base_type->name, "u8", 2) == 0;
                          if (is_cstr) {
                              EMIT((pt->mode == MODE_MUTABLE || pt->mode == MODE_OWNED)
                                   ? "char *" : "const char *");
                          } else
                          // Hack: force const char* for puts/printf
                          if (pt->kind == TYPE_POINTER && pt->element_type->kind == TYPE_SIMPLE &&
                              (
                                // Strings: *char, *u8
                                ((pt->element_type->base_type->length == 4 && strncmp(pt->element_type->base_type->name, "char", 4) == 0) ||
                                 (pt->element_type->base_type->length == 2 && strncmp(pt->element_type->base_type->name, "u8", 2) == 0))
                                ||
                                // FILE handles: *FILE (Shared) -> FILE *
                                (pt->element_type->base_type->length == 4 && strncmp(pt->element_type->base_type->name, "FILE", 4) == 0)
                              ) &&
                              (strcmp(fname, "puts") == 0 || strcmp(fname, "printf") == 0 ||
                               strcmp(fname, "libc_puts") == 0 || strcmp(fname, "libc_printf") == 0 ||
                               strcmp(fname, "fopen") == 0 || strcmp(fname, "fputs") == 0 ||
                               strcmp(fname, "fgets") == 0))
                          {
                              // C-Interop: Map u8* to char* and FILE* to FILE* (mut)
                              Id *base = pt->element_type->base_type;
                              if (base->length == 4 && strncmp(base->name, "FILE", 4) == 0) {
                                  EMIT("FILE *"); // Always mutable FILE* for libc
                              } else {
                                  // Strings (u8*)
                                  if (pt->mode == MODE_MUTABLE || pt->mode == MODE_OWNED) {
                                      EMIT("char *");
                                  } else {
                                      EMIT("const char *");
                                  }
                              }
                          } else {
                              emit_param_type(pt, false);
                          }
                          EMIT(" %.*s",
                               (int)param->decl->as.variable_decl.name->length,
                               param->decl->as.variable_decl.name->name);
                     }
                     first = 0;
                     param = param->next;
                 }
                  if (decl->as.function_decl.is_variadic) {
                      if (!first) EMIT(", ");
                      EMIT("...");
                  }
              } else {
                  if (decl->as.function_decl.is_variadic) {
                      EMIT("...");
                 } else {
                     EMIT("void");
                 }
             }
             EMIT(");\n");
             break;
        }

        case DECL_PROCEDURE:
        case DECL_FUNCTION: {
            // Q-017 [fast_math]: emit pragma to enable FMA fusion for this function
            bool has_fast_math = false;
            for (Attr *a = decl->attributes; a; a = a->next) {
                if (a->name && a->name->length == 9 && strncmp(a->name->name, "fast_math", 9) == 0) {
                    has_fast_math = true;
                    break;
                }
            }
            if (has_fast_math) {
                emit_indent(depth);
                EMIT("#pragma GCC push_options\n");
                emit_indent(depth);
                EMIT("#pragma GCC optimize(\"fp-contract=fast\")\n");
            }

            emit_indent(depth);
            // Q-018 [private]: emit `static` for private declarations (module-internal linkage)
            // skip for main (must be extern)
            const char *fname = decl->as.function_decl.name->name;
            size_t flen = decl->as.function_decl.name->length;
            bool is_main = (flen == 4 && strncmp(fname, "main", 4) == 0);
            if (decl->is_private && !is_main) {
                EMIT("static ");
            }
            // @cold / @hot: programmer-declared frequency hints.
            if (!is_main && decl->as.function_decl.is_cold) EMIT("__attribute__((cold)) ");
            if (!is_main && decl->as.function_decl.is_hot)  EMIT("__attribute__((hot)) ");
            // @allocator: fresh heap pointer — no aliasing with existing data.
            if (!is_main && decl->as.function_decl.is_allocator)
                EMIT("__attribute__((malloc, returns_nonnull)) ");
            // @noreturn: function never returns.
            if (!is_main && decl->as.function_decl.is_noreturn)
                EMIT("__attribute__((noreturn)) ");
            // O-001 [access]: sized-array params → buffer bound annotation.
            if (!is_main) emit_access_attributes(decl);
            // Q-019 [pure/const]: func without var params gets __attribute__((const))
            // when all params are by value (no pointer args → no indirect reads),
            // or __attribute__((pure)) otherwise. Both allow LICM/CSE; const is
            // the stronger guarantee and allows hoisting even when memory changes.
            if (decl->kind == DECL_FUNCTION && !is_main && decl->as.function_decl.return_type &&
                !func_has_var_param(decl) && !func_writes_through_param(decl) &&
                !func_has_fnptr_param(decl)) {
                if (func_all_params_by_value(decl))
                    EMIT("__attribute__((const)) ");
                else
                    EMIT("__attribute__((pure)) ");
            }
            // Q-020 [nonnull]: borrow checker proves every pointer param is non-null.
            if (!is_main && func_has_ptr_param(decl))
                EMIT("__attribute__((nonnull)) ");
            // Q-021 [returns_nonnull]: borrow return type is provably non-null.
            if (!is_main && func_returns_nonnull_ptr(decl))
                EMIT("__attribute__((returns_nonnull)) ");
            // Print return type and function name.
            if (decl->as.function_decl.return_type) {
                emit_type(decl->as.function_decl.return_type);
            } else if (is_main) {
                EMIT("int32_t"); // C99: main must return int
            } else {
                EMIT("void");
            }

            // special case if the function is named "main"
            const char *id_name = decl->as.function_decl.name->name;
            size_t id_len = decl->as.function_decl.name->length;
            if (id_len == 4 && strncmp(id_name, "main", 4) == 0) {
                EMIT(" main(");
            } else {
                EMIT(" %s(", c_name_for_id(decl->as.function_decl.name));
            }

            DeclList* param = decl->as.function_decl.params;
            if (param) {
                int first = 1;
                int param_idx = 0;
                while (param) {
                    if (param->decl->kind == DECL_VARIABLE && param->decl->as.variable_decl.type && param->decl->as.variable_decl.type->kind == TYPE_COMPTIME) {
                        param = param->next;
                        param_idx++;
                        continue;
                    }
                    if (!first) {
                        EMIT(", ");
                    }
                    
                    if (param->decl->kind == DECL_DESTRUCT) {
                        // Emit: Type _param_N
                        emit_param_type(param->decl->as.destruct_decl.type, true);
                        EMIT(" _param_%d", param_idx);
                    } else {
                        Type *pt = param->decl->as.variable_decl.type;
                        Id   *pn = param->decl->as.variable_decl.name;
                        if (pt && pt->kind == TYPE_ARRAY && pt->array_len == -1) {
                            // Fase 7: decompose dynamic array param to (size_t __len_X,)? T * X
                            char elem_buf[256];
                            c_name_for_type(pt->element_type, elem_buf, sizeof elem_buf);
                            if (dynarray_param_has_runtime_len(pt))
                                EMIT("size_t __len_%.*s, ", (int)pn->length, pn->name);
                            if (pt->mode == MODE_MUTABLE || emit_param_is_written(decl, param->decl))
                                EMIT("%s * restrict %.*s", elem_buf, (int)pn->length, pn->name);
                            else
                                EMIT("const %s * restrict %.*s", elem_buf, (int)pn->length, pn->name);
                        } else if (pt && pt->kind == TYPE_FUNC) {
                            // Function-pointer param: name goes inside `R (*name)(P..)`.
                            char nb[128];
                            int nl = pn->length < 127 ? (int)pn->length : 127;
                            memcpy(nb, pn->name, nl); nb[nl] = '\0';
                            char fb[768];
                            c_name_for_fnptr(pt, nb, fb, sizeof fb);
                            EMIT("%s", fb);
                        } else {
                            // Use emit_param_type to print parameter type.
                            emit_param_type(pt, true);
                            EMIT(" %.*s", (int)pn->length, pn->name);
                        }
                    }
                    first = 0;
                    param = param->next;
                    param_idx++;
                }
            } else {
                EMIT("void");
            }
            EMIT(") {\n");

            // Q-022 [refinement-unreachable]: for every parameter with a refinement
            // constraint (e.g. `m usize >= 1`), emit __builtin_unreachable() on the
            // impossible branch. This tells GCC the range of each parameter at
            // zero runtime cost — it eliminates dead branches and enables better
            // prologue/address computation optimization.
            {
                DeclList *rp = decl->as.function_decl.params;
                bool emitted_any = false;
                while (rp) {
                    if (rp->decl && rp->decl->kind == DECL_VARIABLE) {
                        ExprList *cs = rp->decl->as.variable_decl.constraints;
                        // c_name_for_id reuses a static buffer — copy before the
                        // bound (which may call it again for an identifier bound).
                        char pname[160];
                        snprintf(pname, sizeof pname, "%s", c_name_for_id(rp->decl->as.variable_decl.name));
                        for (ExprList *c = cs; c; c = c->next) {
                            Expr *ce = c->expr;
                            if (!ce || ce->kind != EXPR_BINARY) continue;
                            Expr *rhs = ce->as.binary_expr.right;
                            if (!rhs) continue;
                            // The bound is a literal (`x i32 >= 1`) OR another in-scope
                            // identifier — another parameter (`x i32 < k`) or a top-level
                            // constant. Every parameter is in scope in the body, so
                            // referencing one by name is valid C.
                            char bound[160];
                            if (rhs->kind == EXPR_LITERAL)
                                snprintf(bound, sizeof bound, "%lld", (long long)rhs->as.literal_expr.value);
                            else if (rhs->kind == EXPR_IDENTIFIER)
                                snprintf(bound, sizeof bound, "%s", c_name_for_id(rhs->as.identifier_expr.id));
                            else
                                continue;   // only literal or identifier bounds for now
                            TokenKind op = ce->as.binary_expr.op;
                            emit_indent(depth + 1);
                            // The constraint is enforced at every call site, so its
                            // negation (the complementary branch) is unreachable:
                            // param >= b → if (param < b); <= → >; > → <=; < → >=.
                            if (op == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL)       // >= bound
                                EMIT("if (%s < %s) __builtin_unreachable();\n", pname, bound);
                            else if (op == TOKEN_ANGLE_BRACKET_LEFT_EQUAL)   // <= bound
                                EMIT("if (%s > %s) __builtin_unreachable();\n", pname, bound);
                            else if (op == TOKEN_ANGLE_BRACKET_RIGHT)        // > bound
                                EMIT("if (%s <= %s) __builtin_unreachable();\n", pname, bound);
                            else if (op == TOKEN_ANGLE_BRACKET_LEFT)         // < bound
                                EMIT("if (%s >= %s) __builtin_unreachable();\n", pname, bound);
                            emitted_any = true;
                        }
                    }
                    rp = rp->next;
                }
                (void)emitted_any;
            }

            // Sprint C [struct-invariant-assume]: for each struct-type parameter,
            // emit if (!(param->field < bound)) __builtin_unreachable() for every
            // field annotated `field Type in container`.  This propagates the
            // struct invariant into the function body, letting GCC eliminate
            // defensive branches that check the same condition.
            //
            // Case 1: scalar field, dynamic slice container  → if (!(f < c.len)) unreachable
            // Case 2: scalar field, fixed array container   → if (!(f < N))     unreachable
            // Case 3: pointer field, any container          → if (!(p >= c.data && p < c.data+N)) unreachable
            {
                DeclList *sp = decl->as.function_decl.params;
                while (sp) {
                    Decl *pd = sp->decl;
                    sp = sp->next;
                    if (!pd || pd->kind != DECL_VARIABLE) continue;
                    Type *pt = pd->as.variable_decl.type;
                    // Only non-primitive TYPE_SIMPLE (struct types)
                    if (!pt || pt->kind != TYPE_SIMPLE || !pt->base_type) continue;
                    if (is_primitive_type(pt)) continue;

                    // Find the struct definition by matching Lain name.
                    // pt->base_type->name is the Lain name ("Lexer").
                    // After sema resolution, the base_type name might be the
                    // module-qualified Lain name (e.g. "struct_inv_test_Lexer").
                    // Try both: exact match first, then suffix match.
                    Decl *sd = NULL;
                    for (DeclList *dl = emitted_decls; dl; dl = dl->next) {
                        if (!dl->decl || dl->decl->kind != DECL_STRUCT) continue;
                        Id *sn = dl->decl->as.struct_decl.name;
                        if (!sn) continue;
                        // Try exact match by Lain name
                        if (sn->length == pt->base_type->length &&
                            strncmp(sn->name, pt->base_type->name, sn->length) == 0) {
                            sd = dl->decl; break;
                        }
                        // Try C name match: c_name_for_id of struct decl name
                        const char *struct_cname = c_name_for_id(sn);
                        if (struct_cname && strlen(struct_cname) == (size_t)pt->base_type->length &&
                            strncmp(struct_cname, pt->base_type->name, pt->base_type->length) == 0) {
                            sd = dl->decl; break;
                        }
                    }
                    if (!sd) continue;

                    // Copy param C name (c_name_for_id uses a static buffer)
                    const char *raw_pn = c_name_for_id(pd->as.variable_decl.name);
                    char pnbuf[256]; strncpy(pnbuf, raw_pn, 255); pnbuf[255] = '\0';

                    // Struct params are always pointer in C (const T* or T*)
                    // so field access uses -> not .
                    const char *acc = "->";

                    // Scan struct fields for in_field annotations
                    for (DeclList *f = sd->as.struct_decl.fields; f; f = f->next) {
                        if (!f->decl || f->decl->kind != DECL_VARIABLE) continue;
                        Id *in_fld = f->decl->as.variable_decl.in_field;
                        if (!in_fld) continue;

                        Type *fty = f->decl->as.variable_decl.type;
                        if (!fty) continue;
                        // Handle: scalar index (Cases 1+2) and pointer (Case 3)
                        bool is_scalar = (fty->kind == TYPE_SIMPLE);
                        bool is_ptr    = (fty->kind == TYPE_POINTER);
                        if (!is_scalar && !is_ptr) continue;

                        Id *fname = f->decl->as.variable_decl.name;

                        // Find the container field by matching in_fld name
                        Type *cty = NULL; Id *cname_id = NULL;
                        for (DeclList *cf = sd->as.struct_decl.fields; cf; cf = cf->next) {
                            if (!cf->decl || cf->decl->kind != DECL_VARIABLE) continue;
                            Id *cfn = cf->decl->as.variable_decl.name;
                            if (!cfn || cfn->length != in_fld->length) continue;
                            if (strncmp(cfn->name, in_fld->name, in_fld->length) != 0) continue;
                            cty = cf->decl->as.variable_decl.type;
                            cname_id = cfn;
                            break;
                        }
                        if (!cty || !cname_id) continue;

                        // Pre-format names to avoid repeated %.*s verbosity
                        char fnbuf[256], cnbuf[256];
                        snprintf(fnbuf, sizeof fnbuf, "%.*s",
                                 (int)fname->length, fname->name);
                        snprintf(cnbuf, sizeof cnbuf, "%.*s",
                                 (int)cname_id->length, cname_id->name);

                        emit_indent(depth + 1);
                        if (is_scalar) {
                            if (cty->kind == TYPE_ARRAY && cty->array_len == -1) {
                                // Case 1: dynamic slice *T[] — bound = container.len
                                EMIT("if (!(%s%s%s < %s%s%s.len)) __builtin_unreachable();\n",
                                     pnbuf, acc, fnbuf, pnbuf, acc, cnbuf);
                            } else if (cty->kind == TYPE_ARRAY && cty->array_len >= 0) {
                                // Case 2: fixed array T[N] — bound = N (compile-time)
                                EMIT("if (!(%s%s%s < %lld)) __builtin_unreachable();\n",
                                     pnbuf, acc, fnbuf, (long long)cty->array_len);
                            }
                        } else {
                            // Case 3: pointer field — two-sided bound check
                            if (cty->kind == TYPE_ARRAY && cty->array_len == -1) {
                                // slice container: ptr >= arr.data && ptr < arr.data + arr.len
                                EMIT("if (!(%s%s%s >= %s%s%s.data && %s%s%s < %s%s%s.data + %s%s%s.len)) __builtin_unreachable();\n",
                                     pnbuf, acc, fnbuf,
                                     pnbuf, acc, cnbuf,
                                     pnbuf, acc, fnbuf,
                                     pnbuf, acc, cnbuf,
                                     pnbuf, acc, cnbuf);
                            } else if (cty->kind == TYPE_ARRAY && cty->array_len >= 0) {
                                // fixed array container: ptr >= base && ptr < base + N
                                if (is_user_type_fixed_array(cty)) {
                                    // native C array T arr[N]: decays to pointer
                                    EMIT("if (!(%s%s%s >= %s%s%s && %s%s%s < %s%s%s + %lld)) __builtin_unreachable();\n",
                                         pnbuf, acc, fnbuf,
                                         pnbuf, acc, cnbuf,
                                         pnbuf, acc, fnbuf,
                                         pnbuf, acc, cnbuf,
                                         (long long)cty->array_len);
                                } else {
                                    // Fixed_T_N struct: .data is the array field
                                    EMIT("if (!(%s%s%s >= %s%s%s.data && %s%s%s < %s%s%s.data + %lld)) __builtin_unreachable();\n",
                                         pnbuf, acc, fnbuf,
                                         pnbuf, acc, cnbuf,
                                         pnbuf, acc, fnbuf,
                                         pnbuf, acc, cnbuf,
                                         (long long)cty->array_len);
                                }
                            }
                        }
                    }
                }
            }

            // Inject destructuring initialization
            param = decl->as.function_decl.params;
            int param_idx = 0;
            while (param) {
                if (param->decl->kind == DECL_DESTRUCT) {
                    DeclDestruct *dd = &param->decl->as.destruct_decl;
                    
                    // Resolve struct to find field types
                    Decl *struct_decl = NULL;
                    if (dd->type->kind == TYPE_SIMPLE) {
                        for (DeclList *g = emitted_decls; g; g = g->next) {
                            if (g->decl->kind == DECL_STRUCT) {
                                Id *sname = g->decl->as.struct_decl.name;
                                if (sname->length == dd->type->base_type->length &&
                                    strncmp(sname->name, dd->type->base_type->name, sname->length) == 0) {
                                    struct_decl = g->decl;
                                    break;
                                }
                            }
                        }
                    }

                    if (struct_decl) {
                        for (IdList *n = dd->names; n; n = n->next) {
                            // Find field type
                            Type *field_type = NULL;
                            for (DeclList *f = struct_decl->as.struct_decl.fields; f; f = f->next) {
                                Id *fname = f->decl->as.variable_decl.name;
                                if (fname->length == n->id->length &&
                                    strncmp(fname->name, n->id->name, fname->length) == 0) {
                                    field_type = f->decl->as.variable_decl.type;
                                    break;
                                }
                            }
                            
                            if (field_type) {
                                emit_indent(depth + 1);
                                emit_type(field_type);
                                
                                // Check if passed by value (OWNED) or pointer (SHARED/MUTABLE)
                                const char *op = "->";
                                if (dd->type->mode == MODE_OWNED) {
                                    op = ".";
                                }

                                EMIT(" %.*s = _param_%d%s%.*s;\n",
                                     (int)n->id->length, n->id->name,
                                     param_idx,
                                     op,
                                     (int)n->id->length, n->id->name);
                            }
                        }
                    }
                }
                param = param->next;
                param_idx++;
            }

            emit_stmt_list(decl->as.function_decl.body, depth + 1);
            emit_indent(depth);
            EMIT("}\n");

            // Q-017 [fast_math]: pop pragma options after function body
            if (has_fast_math) {
                emit_indent(depth);
                EMIT("#pragma GCC pop_options\n");
            }
            EMIT("\n");
            break;
        }


        case DECL_STRUCT: {
            const char *structName = c_name_for_id(decl->as.struct_decl.name);

            // Sprint 19: [packed] struct — bit-exact layout.
            // Each iN/uN field occupies exactly N bits within a scalar
            // container. We emit a typedef to the smallest stdint type
            // that fits the total bit-width and inline getter functions
            // that extract each field via shift+mask.
            if (decl->as.struct_decl.is_packed) {
                // Pass 1: compute total bit-width and per-field offsets.
                int total_bits = 0;
                struct { int bits; int offset; bool ok; } pf[64];
                int n_fields = 0;
                bool fields_ok = true;
                for (DeclList *f = decl->as.struct_decl.fields; f && n_fields < 64; f = f->next) {
                    if (!f->decl || f->decl->kind != DECL_VARIABLE) {
                        fields_ok = false; break;
                    }
                    Type *ft = f->decl->as.variable_decl.type;
                    if (!ft || ft->kind != TYPE_SIMPLE || !ft->base_type) {
                        fields_ok = false; break;
                    }
                    const char *fname = ft->base_type->name;
                    isize flen = ft->base_type->length;
                    if (flen < 2 || flen > 3 || (fname[0] != 'i' && fname[0] != 'u')) {
                        fields_ok = false; break;
                    }
                    int n = 0; bool ok_digits = true;
                    for (isize k = 1; k < flen; k++) {
                        if (fname[k] < '0' || fname[k] > '9') { ok_digits = false; break; }
                        n = n * 10 + (fname[k] - '0');
                    }
                    if (!ok_digits || n < 1 || n > 64) {
                        fields_ok = false; break;
                    }
                    pf[n_fields].bits = n;
                    pf[n_fields].offset = total_bits;
                    pf[n_fields].ok = true;
                    total_bits += n;
                    n_fields++;
                }
                if (!fields_ok || total_bits == 0 || total_bits > 64) {
                    fprintf(stderr,
                        "[E121] [packed] struct '%s' invalid: only iN/uN fields with total ≤ 64 bits supported (got %d bits).\n",
                        structName, total_bits);
                    exit(1);
                }
                const char *container =
                    (total_bits <= 8)  ? "uint8_t"  :
                    (total_bits <= 16) ? "uint16_t" :
                    (total_bits <= 32) ? "uint32_t" :
                                         "uint64_t";
                int container_bits =
                    (total_bits <= 8)  ?  8 :
                    (total_bits <= 16) ? 16 :
                    (total_bits <= 32) ? 32 : 64;
                (void)container_bits;

                emit_indent(depth);
                EMIT("typedef %s %s;\n", container, structName);
                register_struct_type(structName);

                // Constructor: takes all fields, returns packed container.
                emit_indent(depth);
                EMIT("static inline %s %s_ctor(", structName, structName);
                int idx = 0;
                int first = 1;
                for (DeclList *f = decl->as.struct_decl.fields; f; f = f->next, idx++) {
                    if (!first) EMIT(", ");
                    emit_type(f->decl->as.variable_decl.type);
                    EMIT(" %.*s",
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name);
                    first = 0;
                }
                EMIT(") {\n");
                emit_indent(depth + 1);
                EMIT("return (%s)(", container);
                idx = 0; first = 1;
                for (DeclList *f = decl->as.struct_decl.fields; f; f = f->next, idx++) {
                    if (!first) EMIT(" | ");
                    int bits = pf[idx].bits;
                    int off  = pf[idx].offset;
                    unsigned long long mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
                    EMIT("(((%s)(%.*s) & 0x%llxULL) << %d)",
                         container,
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name,
                         mask, off);
                    first = 0;
                }
                EMIT(");\n");
                emit_indent(depth);
                EMIT("}\n\n");

                // Per-field getter functions.
                // (Setters were removed for philosophical coherence with P5
                //  "no hidden magic": `r.pin1 = 9` looks like assignment but
                //  was rewritten as an inline function call. To modify a
                //  packed struct field, reconstruct the struct explicitly:
                //  `r = GpioModer(r.pin0, 9, r.pin2, r.pin3)`. This makes
                //  the cost and semantics visible at the call site.)
                idx = 0;
                for (DeclList *f = decl->as.struct_decl.fields; f; f = f->next, idx++) {
                    int bits = pf[idx].bits;
                    int off  = pf[idx].offset;
                    unsigned long long mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);

                    // Getter: extract bit range.
                    emit_indent(depth);
                    EMIT("static inline ");
                    emit_type(f->decl->as.variable_decl.type);
                    EMIT(" %s_get_%.*s(%s r) {\n",
                         structName,
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name,
                         structName);
                    emit_indent(depth + 1);
                    EMIT("return (");
                    emit_type(f->decl->as.variable_decl.type);
                    EMIT(")((r >> %d) & 0x%llxULL);\n", off, mask);
                    emit_indent(depth);
                    EMIT("}\n\n");
                }
                break;
            }

            // 1) struct definition
            emit_indent(depth);
            EMIT("typedef struct %s {\n", structName);

            // 2) fields
            for (DeclList* field = decl->as.struct_decl.fields; field; field = field->next) {
                if (field->decl) {
                    emit_indent(depth + 1);
                    emit_type(field->decl->as.variable_decl.type);
                    EMIT(" %.*s;\n",
                         (int)field->decl->as.variable_decl.name->length,
                         field->decl->as.variable_decl.name->name);
                } else {
                    emit_indent(depth + 1);
                    EMIT("/* NULL field in struct %s */\n", structName);
                }
            }

            // 3) close typedef
            emit_indent(depth);
            EMIT("} %s;\n\n", structName);
            register_struct_type(structName);

            // 4) inline “constructor” function
            //    static inline StructName StructName_ctor(field1_type f1, field2_type f2, …) { … }
            emit_indent(depth);
            EMIT("static inline %s %s_ctor(", structName, structName);
            // parameters
            {
                int first = 1;
                for (DeclList* f = decl->as.struct_decl.fields; f; f = f->next) {
                    if (!f->decl) continue;
                    if (!first) EMIT(", ");
                    emit_type(f->decl->as.variable_decl.type);
                    EMIT(" %.*s",
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name);
                    first = 0;
                }
            }
            EMIT(") {\n");

            // body: return (StructName){ .field1 = field1, .field2 = field2, … };
            emit_indent(depth + 1);
            EMIT("return (%s){ ", structName);
            {
                int first = 1;
                for (DeclList* f = decl->as.struct_decl.fields; f; f = f->next) {
                    if (!f->decl) continue;
                    if (!first) EMIT(", ");
                    EMIT(".%.*s = %.*s",
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name,
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name);
                    first = 0;
                }
            }
            EMIT(" };\n");

            // 5) close constructor function
            emit_indent(depth);
            EMIT("}\n\n");

            break;
        }



        case DECL_ENUM: {
            // 1) lookup the C‐enum tag, e.g. "main_Shape"
            const char *adt_name = c_name_for_id(decl->as.enum_decl.type_name);

            // D-Niche (re-land): if the payload has enough sentinel space, emit a
            // bare typedef to the backing type (NO tag byte) plus sentinel/identity
            // constructors, then skip the legacy tag+union path entirely.
            NicheLayout __niche_layout = {0};
            if (enum_is_zero_cost_niche(decl, &__niche_layout)) {
                bool is_multi = (__niche_layout.primary_variant != NULL &&
                                 __niche_layout.secondary_variant != NULL);

                // The scalar typedef + registration were emitted in the
                // forward-decl pass (emit.h) so the name is complete before the
                // function forward decls that pass it by `const T*`. Here we emit
                // only constructors; `backing` still drives the return casts.
                char backing[256];
                emit_niche_backing_type(decl, &__niche_layout, backing, sizeof backing);

                // Constructors: empty variants return their sentinel; the single
                // payload variant is the identity; a multi secondary maps its
                // sub-enum ordinal into the primary's sentinel pool (value*stride).
                for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                    emit_indent(depth);
                    EMIT("static inline %s %s_%.*s(", adt_name, adt_name,
                         (int)v->name->length, v->name->name);
                    if (v->fields) {
                        int first = 1;
                        for (DeclList *f = v->fields; f; f = f->next) {
                            if (!first) EMIT(", ");
                            emit_type(f->decl->as.variable_decl.type);
                            EMIT(" %.*s",
                                 (int)f->decl->as.variable_decl.name->length,
                                 f->decl->as.variable_decl.name->name);
                            first = 0;
                        }
                    } else {
                        EMIT("void");
                    }
                    EMIT(") {\n");
                    emit_indent(depth + 1);

                    if (is_multi && v == __niche_layout.secondary_variant) {
                        Id *fname = v->fields->decl->as.variable_decl.name;
                        long long stride = __niche_layout.pool.ptr_stride > 0
                            ? __niche_layout.pool.ptr_stride : 1;
                        if (__niche_layout.pool.kind == POOL_POINTER) {
                            EMIT("return (%s)(uintptr_t)((long long)%.*s * %lldLL);\n",
                                 backing, (int)fname->length, fname->name, stride);
                        } else {
                            EMIT("return (%s)((long long)%.*s * %lldLL);\n",
                                 backing, (int)fname->length, fname->name, stride);
                        }
                    } else if (!v->fields) {
                        long long s = niche_sentinel_for_variant(
                            &decl->as.enum_decl, v, &__niche_layout);
                        if (__niche_layout.pool.kind == POOL_POINTER) {
                            EMIT("return (%s)(uintptr_t)%lldLL;\n", backing, s);
                        } else {
                            EMIT("return (%s)%lldLL;\n", backing, s);
                        }
                    } else {
                        Id *fname = v->fields->decl->as.variable_decl.name;
                        if (__niche_layout.pool.kind == POOL_BOOL && !is_multi) {
                            EMIT("return (uint8_t)%.*s;\n",
                                 (int)fname->length, fname->name);
                        } else {
                            EMIT("return %.*s;\n", (int)fname->length, fname->name);
                        }
                    }
                    emit_indent(depth);
                    EMIT("}\n\n");
                }
                break;  // skip legacy tag+union emit
            }

            // 2) Generate the Tag Enum: typedef enum { Shape_Tag_Circle, Shape_Tag_Rectangle } Shape_Tag;
            emit_indent(depth);
            EMIT("typedef enum {\n");

            for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                emit_indent(depth + 1);
                EMIT("%s_Tag_%.*s,\n", adt_name, (int)v->name->length, v->name->name);
            }

            emit_indent(depth);
            EMIT("} %s_Tag;\n\n", adt_name);

            // 3) Generate the ADT Struct: typedef struct Shape { Shape_Tag tag; union { ... } data; } Shape;
            emit_indent(depth);
            EMIT("typedef struct %s {\n", adt_name);
            emit_indent(depth + 1);
            EMIT("%s_Tag tag;\n", adt_name);
            
            // Only generate union if there are variants with fields
            bool has_fields = false;
            for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                if (v->fields) {
                    has_fields = true;
                    break;
                }
            }
            
            if (has_fields) {
                emit_indent(depth + 1);
                EMIT("union {\n");
                
                for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                    if (v->fields) {
                        emit_indent(depth + 2);
                        EMIT("struct {\n");
                        for (DeclList *f = v->fields; f; f = f->next) {
                            emit_indent(depth + 3);
                            emit_type(f->decl->as.variable_decl.type);
                            EMIT(" %.*s;\n", 
                                 (int)f->decl->as.variable_decl.name->length,
                                 f->decl->as.variable_decl.name->name);
                        }
                        emit_indent(depth + 2);
                        EMIT("} %.*s;\n", (int)v->name->length, v->name->name);
                    }
                }
                
                emit_indent(depth + 1);
                EMIT("} data;\n");
            }
            
            emit_indent(depth);
            EMIT("} %s;\n\n", adt_name);
            
            register_struct_type(adt_name); // Register as a type so it can be used

            // 4) Generate Constructors
            // static inline Shape Shape_Circle(int radius) { return (Shape){ .tag = Shape_Tag_Circle, .data.Circle = { radius } }; }
            for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                emit_indent(depth);
                EMIT("static inline %s %s_%.*s(", adt_name, adt_name, (int)v->name->length, v->name->name);
                
                // Params
                if (v->fields) {
                    int first = 1;
                    for (DeclList *f = v->fields; f; f = f->next) {
                        if (!first) EMIT(", ");
                        emit_type(f->decl->as.variable_decl.type);
                        EMIT(" %.*s", 
                             (int)f->decl->as.variable_decl.name->length,
                             f->decl->as.variable_decl.name->name);
                        first = 0;
                    }
                }
                
                EMIT(") {\n");
                emit_indent(depth + 1);
                EMIT("return (%s){ .tag = %s_Tag_%.*s", adt_name, adt_name, (int)v->name->length, v->name->name);
                
                if (v->fields) {
                    EMIT(", .data.%.*s = { ", (int)v->name->length, v->name->name);
                    int first = 1;
                    for (DeclList *f = v->fields; f; f = f->next) {
                        if (!first) EMIT(", ");
                        EMIT(".%.*s = %.*s", 
                             (int)f->decl->as.variable_decl.name->length, f->decl->as.variable_decl.name->name,
                             (int)f->decl->as.variable_decl.name->length, f->decl->as.variable_decl.name->name);
                        first = 0;
                    }
                    EMIT(" }");
                }
                
                EMIT(" };\n");
                emit_indent(depth);
                EMIT("}\n\n");
            }
        }
        break;


        case DECL_C_INCLUDE: {
            const char* path = decl->as.c_include_decl.path;
            emit_indent(depth);
            if (path[0] == '<') {
                EMIT("#include %s\n", path);
            } else {
                EMIT("#include \"%s\"\n", path);
            }
            break;
        }

        case DECL_EXTERN_TYPE: {
            const char* name = c_name_for_id(decl->as.extern_type_decl.name);
            emit_indent(depth);
            // In C, "typedef struct Name Name;" allows using "Name" as an opaque type
            EMIT("typedef struct %s %s;\n", name, name);
            register_struct_type(name); 
            break;
        }

        case DECL_TYPE_ALIAS: {
            // Refinement aliases like `type Pressure = i32 >= 0 and <= 1000`
            // get a typedef so downstream usage can name them. May have
            // been pre-emitted in the forward stage (see emit.h); skip
            // duplicates via is_struct_type().
            Expr *rhs = decl->as.type_alias_decl.expr;
            if (rhs && rhs->kind == EXPR_TYPE && rhs->as.type_expr.type_value) {
                Type *under = rhs->as.type_expr.type_value;
                if (under->kind == TYPE_SIMPLE && under->base_type) {
                    const char *alias_cn = c_name_for_id(decl->as.type_alias_decl.name);
                    if (!is_struct_type(alias_cn)) {
                        char back[128];
                        c_name_for_type(under, back, sizeof back);
                        emit_indent(depth);
                        EMIT("typedef %s %s;\n", back, alias_cn);
                        register_scalar_typedef(alias_cn);
                    }
                }
            }
            break;
        }

        case DECL_IMPORT:
            // Imports are handled by frontend resolution, nothing to emit directly in C
            // (unless we decide to emit #include "module.h" later, but for now single file/unity build assumed or managed externally)
            break;
            
        case DECL_DESTRUCT:
            break; // handled in function params

        default:
            emit_indent(depth);
            EMIT("/* Unhandled declaration type %d */\n", decl->kind);
            break;
    }
}

#endif // EMIT_DECL_H
