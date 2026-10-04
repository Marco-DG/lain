// src/frontends/lain/lower.h — the Lain AST → Lain-IR lowering (Phase 1.2).
//
// ★ MOVED OUT OF src/ir/ ON 2026-09-24, and the move is the point. This file references AST
// types and is included AFTER ast.h by its driver — it was never core IR, it is the Lain front
// end's CLIENT of the IR, exactly as src/frontends/cmin/cmin.c contains its own lowering for
// the C subset. Filing it under ir/ said the IR knew about Lain. It does not.
//
// Walks the frontend's TYPED AST (after sema_resolve_module populates ->type) and
// emits the IR of build.h. Mutable and immutable locals both become stack slots
// (alloca/load/store) — the memory form; mem2reg (a later pass) promotes them to SSA.
// This is the ONLY place source names are read; downstream everything is value
// identity. Coverage is the core subset (grown toward the Phase-1 gate); unhandled
// constructs emit a typed `const 0` placeholder so lowering completes and the CFG is
// inspectable. Not wired into main.c.
#ifndef LAIN_IR_LOWER_H
#define LAIN_IR_LOWER_H

#include "ir/build.h"

// ── local environment (the name→storage map — the AST/IR boundary) ───────────
typedef struct IrLocal {
    Id      *name;
    IrValue *slot;        // alloca address for a scalar local (load/store), OR the
                          // element-base pointer for an aggregate local
    IrValue *param;       // param value (read directly; NULL if it's a slot local)
    bool     aggregate;   // array/slice/struct: `slot` is the base, read it directly
    struct IrLocal *next;
} IrLocal;

typedef struct {
    IrFunc   *f;
    IrBlock  *cur;         // block currently being filled
    Arena    *a;
    Decl     *fdecl;       // the function being lowered (for its own return ensures)
    IrLocal  *locals;
    bool      unsafe;      // inside an `unsafe` block (elem_ptr etc. become unchecked)
    // innermost loop targets, for break/continue
    IrBlock  *loop_head, *loop_exit;
    // A `for` loop's STEP, which `continue` must run. It targets the LATCH (step the counter,
    // jump to the header); it targeted the header, so `for i in 0..3 { if i == 1 { continue } }`
    // never left i == 1: refused as E011 in a total function, a hang in one that may diverge.
    // The latch is made at the first `continue`, so a loop without one is lowered as before.
    // NULL cell: a `while`, whose `continue` is its header.
    IrValue  *loop_step_cell;
    IrType   *loop_step_ty;
    IrBlock  *loop_latch;
    int       loop_defer_mark;   // defer-stack depth on entering the innermost loop BODY:
                                 // `break`/`continue` leave that block, so they run what it
                                 // registered, exactly as falling off its end does.
    DeclList *globals;     // module top-level decls (for global-constant references)
    int       const_depth; // recursion guard for cyclic constant initializers
    Expr     *table_read;  // the base of the `T[i]` being READ: the one place a module constant
                           // table is its read-only static object (ir_data_for_global)
    // struct-type memo: cache the IrType per struct decl so a self-referential
    // field (a pointer back to the struct) returns the in-progress node instead of
    // recursing forever.
    Decl     *scache_decl[64];
    IrType   *scache_type[64];
    int       scache_n;
    // `defer` stack. Lowering records the deferred statements here and REPLAYS them at each
    // exit of the block that registered them (ir_lower_stmts; a `return` replays them all).
    // Without this, defer was simply dropped and the whole function marked `incomplete`,
    // which made a deferred consumption invisible: `defer drop(mov r); drop(mov r)` is a
    // double free that no analysis could see because the function was never analysed. It
    // grows from the arena: a fixed 64 ran only the first 64 of 65.
    Stmt    **defers;
    int       ndefers, defers_cap;
    bool      in_defer;    // replaying a deferred statement: control cannot leave it (E138)
    // ── D-49: THE GUARD'S VALUES, SO THE MEASURE CAN REUSE THEM ─────────────────────────
    // `while i < n / 2 decreasing n / 2 - i` mentions `n / 2` twice. Lowering each occurrence
    // independently makes two SSA values, and a RELATIONAL DOMAIN RELATES VALUES, NOT SYNTAX:
    // the octagon knows `i < %k` from the guard and is asked about `%m - i` from the measure,
    // with nothing tying `%m` to `%k`. So the measure's subtraction cannot be shown not to
    // underflow, and an ordinary in-place reverse is refused.
    //
    // While the loop's CONDITION is lowered, each pure subexpression is recorded here; the
    // MEASURE then reuses a structurally identical one instead of recomputing it. That is
    // local value numbering restricted to one statement, which is the smallest thing that
    // makes the two spellings denote one value — and it is a lowering concern, not a domain
    // one: the domain was right to refuse two unrelated values.
    Expr     *cse_expr[32];
    IrValue  *cse_val[32];
    int       cse_n;
    bool      cse_recording;   // only while lowering a `while` condition
} LowerCtx;

// ── A FUNCTION'S IDENTITY IS MODULE-QUALIFIED, AND THE IR HAS TO CARRY IT (D-54) ─────────
// Lain's modules share a flat namespace, but a QUALIFIED call is legal — so `lib/a.ln` and
// `lib/b.ln` may both define `value`, both reach the IR, and both arrive under the bare name.
// Every analysis resolves a callee by name and takes the FIRST match (`vra_find_func`,
// `bor_find_func`, `di_find_func`, the effect row's own lookup), so one of them is analysed as
// the other. Measured with the project's own driver:
//
//     UNSOUND main  old={IO} new={}  (dropped observable IO)
//
// — `b.value` prints, the lookup finds `a.value` which is pure, and `main` comes out pure and
// total. That is the miscompile class this project has already shipped once.
//
// It is not a shipping miscompile TODAY only because the emitted C does not compile: both
// backends collide on the name. Which is the dangerous part — the C compiler is the only thing
// standing between this and a wrong answer, and fixing the emitted SPELLING is exactly what
// would remove it. So identity comes first, and the spelling follows from it.
//
// This is not a C concern. Chapter 0 says the IR is the authority on a program's meaning, and
// two functions with different bodies are different functions whatever a backend calls them.
// `defining_module` is the front end's own record of where a decl came from — and note it is
// the DEFINING module, which is the bug in the old emitter's scheme: that one prefixes the
// ROOT module, so two modules' `value` both became `main_value`.
static IrName *ir_qualified_name(Arena *a, Decl *d, Id *nm) {
    if (!nm) return NULL;
    // `main` is the program's ENTRY POINT and its identity is fixed by the platform, not by
    // the module it happens to sit in — there is exactly one and it cannot collide. Qualifying
    // it produced `main_main` and an emitted C file with no `main` at all.
    if (nm->length==4 && memcmp(nm->name,"main",4)==0) return ir_intern(a, nm->name, nm->length);
    // ★ AN EXTERN NAMES A FOREIGN SYMBOL, and its name is no more ours to choose than its ABI
    // was (see the emitter's extern note). `libc_puts` is `puts` in someone else's object file;
    // qualifying it produced a link error against `lib_b_libc_puts`. The rule is the same one,
    // arriving from the other side: what crosses the boundary keeps the boundary's spelling.
    if (d && (d->kind==DECL_EXTERN_FUNCTION))
        return ir_intern(a, nm->name, nm->length);
    const char *mod = d ? d->defining_module : NULL;
    if (!mod || !*mod) return ir_intern(a, nm->name, nm->length);   // the root module
    size_t ml = strlen(mod), nl = (size_t)nm->length;
    char *buf = arena_push_many(a, char, ml + 1 + nl + 1);
    size_t k = 0;
    // Sanitise EVERY character that cannot appear in an identifier, not just the dot. A
    // defining_module is a PATH — `tests/codegen/const_attr_pointer_reader_pass` — and
    // replacing only `.` emitted names with slashes in them, which 251 programs then failed to
    // compile. The old emitter's `c_name_for_id` has always done the general form; taking the
    // specific case from a dotted example was the mistake.
    for (size_t i = 0; i < ml; i++) {
        char ch = mod[i];
        bool ok = (ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_';
        buf[k++] = ok ? ch : '_';
    }
    buf[k++] = '_';
    memcpy(buf + k, nm->name, nl); k += nl;
    buf[k] = '\0';
    return ir_intern(a, buf, (isize)k);
}

// Structural equality over the PURE expression shapes a loop guard and its measure share.
// Deliberately narrow: an identifier, a literal, `x.len`, and the arithmetic that joins them.
// Anything with a call, an index or a side effect returns false, so nothing observable is ever
// de-duplicated — reuse is only ever a way to name the SAME value twice.
static bool ir_expr_same(const Expr *a, const Expr *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
        case EXPR_LITERAL:
            return a->as.literal_expr.value == b->as.literal_expr.value;
        case EXPR_IDENTIFIER:
            return a->as.identifier_expr.id && b->as.identifier_expr.id
                && a->as.identifier_expr.id->length == b->as.identifier_expr.id->length
                && memcmp(a->as.identifier_expr.id->name, b->as.identifier_expr.id->name,
                          (size_t)a->as.identifier_expr.id->length) == 0;
        case EXPR_MEMBER:
            return a->as.member_expr.member && b->as.member_expr.member
                && a->as.member_expr.member->length == b->as.member_expr.member->length
                && memcmp(a->as.member_expr.member->name, b->as.member_expr.member->name,
                          (size_t)a->as.member_expr.member->length) == 0
                && ir_expr_same(a->as.member_expr.target, b->as.member_expr.target);
        case EXPR_BINARY:
            return a->as.binary_expr.op == b->as.binary_expr.op
                && ir_expr_same(a->as.binary_expr.left,  b->as.binary_expr.left)
                && ir_expr_same(a->as.binary_expr.right, b->as.binary_expr.right);
        default:
            return false;                       // calls, indexes, anything effectful: never
    }
}

// Mark the function unfaithful, WITH A REASON AND A POSITION: the program is refused there
// (ir_emit_refuse_opaque). First reason wins: it is the first construct that defeated lowering.
static void ir_incomplete(LowerCtx *c, const char *why) {
    if (!c || !c->f) return;
    c->f->incomplete = true;
    if (!c->f->incomplete_why) {
        c->f->incomplete_why = why;
        c->f->incomplete_line = ir_cur_line; c->f->incomplete_col = ir_cur_col;
    }
}

// B3: lower an unmodelled EXPRESSION as an honest opaque instead of poisoning the whole
// function. The value is unknown and the footprint is declared, so analyses havoc exactly
// that and keep analysing everything else. `incomplete` is now reserved for what an opaque
// cannot express — unmodelled CONTROL FLOW, where we cannot even say which paths exist.
static IrValue *ir_opaque_expr(LowerCtx *c, IrType *ty, bool writes, const char *why,
                               IrValue *r0, IrValue *r1) {
    IrValue *reads[2]; int n=0;
    if (r0) reads[n++]=r0;
    if (r1) reads[n++]=r1;
    return ir_opaque(c->f, c->cur, ty ? ty : ir_type_int(c->a,32,true), writes, why, reads, n);
}

// A block's terminator is "set" once lowering has given it one. A freshly-memset
// block has kind==IR_TERM_BR (0) with a==NULL, which is the unset sentinel.
static bool ir_is_set_term(IrBlock *b) {
    return b->term.kind != IR_TERM_BR || b->term.a != NULL;
}

static IrLocal *ir_env_find(LowerCtx *c, Id *name) {
    for (IrLocal *l = c->locals; l; l = l->next)
        if (l->name && name && l->name->length == name->length &&
            strncmp(l->name->name, name->name, (size_t)name->length) == 0) return l;
    return NULL;
}
// ★ A REFERENCE BINDING NAMES WHAT ITS SLOT POINTS AT. `var r = var p.x` and `var r = get(var d)`
// bind r to a BORROW of a copied type, so r's slot holds an ADDRESS (`*var i32`, marked
// `borrowed` by ir_lower_borrow_binding_type). Only the write path knew that: `r = 9` stored
// through the pointer, but every READ of r loaded the slot and used the POINTER as the value —
// `twice(r)` passed an address truncated to i32 (128 instead of 10), `var k = r` copied one
// (84 instead of 4), `return r +% 1` did arithmetic on it, and `q.x` on a struct binding took a
// field of the slot and emitted C gcc rejects. A `var` PARAMETER never had the defect because
// its pointer IS its slot. The binding keeps its slot (borrow.h's loan liveness is keyed on it),
// and every use goes one level further instead — `ir_lower_addr` yields the referent's address,
// a read loads from there. This predicate is the one place that says which locals those are:
// a raw `*var T` local is not borrowed, and a `var` parameter's slot holds the value's type.
static IrType *ir_ref_binding_ptr(const IrLocal *l) {
    IrType *sv = (l && l->slot && !l->aggregate && l->slot->type) ? l->slot->type->elem : NULL;
    return (sv && sv->kind == IRT_PTR && sv->ptr_mut && sv->borrowed && sv->elem) ? sv : NULL;
}
static void ir_env_add(LowerCtx *c, Id *name, IrValue *slot, IrValue *param) {
    // the slot's source name, for diagnostics and dumps only (never an analysis key)
    if (slot && name && !slot->src_name) slot->src_name = ir_intern(c->a, name->name, name->length);
    IrLocal *l = arena_push_aligned(c->a, IrLocal);
    l->name = name; l->slot = slot; l->param = param; l->aggregate = false;
    l->next = c->locals; c->locals = l;
}
// A module-level immutable constant `NAME T = expr` (scalar) referenced by name.
// sema qualifies a reference as `<defining_module>_<name>` (Q-018), while the decl
// keeps the bare name — so match both the bare and the module-qualified spelling.
// Does `name` begin with the MANGLED form of module path `mod`, followed by '_'? Sema
// qualifies a cross-module reference as `<module>_<name>` with every separator flattened to
// '_' (`tests/_tmp/glob` → `tests__tmp_glob`), while `defining_module` keeps the raw path. A
// literal strncmp therefore never matched for any module whose path contains a separator —
// which is every one of them — so a module-level constant silently fell through to an OPAQUE
// read: `func nib(c u8) u8 { return c & NIB }` had an unknown operand and could prove nothing.
static bool ir_mod_prefix(const char *mod, const Id *name, size_t *out_ml) {
    if (!mod || !name) return false;
    size_t i = 0;
    for (; mod[i]; i++) {
        if ((size_t)name->length <= i) return false;
        char a = mod[i], b = name->name[i];
        if (a=='/' || a=='\\' || a=='.' || a=='-') a = '_';
        if (a != b) return false;
    }
    if ((size_t)name->length <= i || name->name[i] != '_') return false;
    *out_ml = i;
    return true;
}

// F1: index of the parameter `nm` names in this declaration, or −1.
static int ir_param_index_by_name(Decl *fn, Id *nm) {
    if (!fn || !nm) return -1;
    int i = 0;
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next, i++) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Id *pn = p->decl->as.variable_decl.name;
        if (pn && pn->length == nm->length && strncmp(pn->name, nm->name, (size_t)nm->length)==0)
            return i;
    }
    return -1;
}
// The read-only static object for a module constant table (see the materialisation in
// ir_lower_expr), made once per declaration. NULL when the table is not one: an element that
// is not an integer (or bool), or a value the module-assert evaluator cannot compute.
#define IR_DATA_MAX 256
static Decl   *ir_data_decl[IR_DATA_MAX];
static IrData *ir_data_obj[IR_DATA_MAX];
static int     ir_data_n = 0;
static IrData *ir_data_for_global(LowerCtx *c, Decl *g, IrType *ty) {
    for (int k = 0; k < ir_data_n; k++) if (ir_data_decl[k] == g) return ir_data_obj[k];
    if (ir_data_n >= IR_DATA_MAX || !ty || ty->kind != IRT_ARRAY || ty->array_len <= 0 || !ty->elem ||
        (ty->elem->kind != IRT_INT && ty->elem->kind != IRT_BOOL)) return NULL;
    Expr *init = g->as.variable_decl.init;
    int n = 0; for (ExprList *el = init->as.array_literal_expr.elements; el; el = el->next) n++;
    if (n != ty->array_len) return NULL;
    int64_t *vals = arena_push_many_aligned(c->a, int64_t, n);
    int k = 0;
    for (ExprList *el = init->as.array_literal_expr.elements; el; el = el->next, k++) {
        Expr *x = el->expr;
        __int128 v; bool lay = false;
        if (!x || !sa_is_const(x, &lay) || lay || !sa_eval(x, &v) ||
            v < (__int128)INT64_MIN || v > (__int128)INT64_MAX) return NULL;
        vals[k] = (int64_t)v;
    }
    IrData *d = arena_push_aligned(c->a, IrData);
    memset(d, 0, sizeof *d);
    Id *nm = g->as.variable_decl.name;
    IrName *q = ir_qualified_name(c->a, g, nm);
    char buf[300]; int bl = snprintf(buf, sizeof buf, "lain_ro_%.*s", q ? (int)q->length : 0, q ? q->name : "");
    for (int j = 0; j < bl; j++) if (!((buf[j] >= 'a' && buf[j] <= 'z') || (buf[j] >= 'A' && buf[j] <= 'Z') ||
                                       (buf[j] >= '0' && buf[j] <= '9') || buf[j] == '_')) buf[j] = '_';
    d->name = ir_intern(c->a, buf, bl);
    d->type = ty; d->vals = vals; d->n = n;
    d->lo = d->hi = vals[0];
    for (int j = 1; j < n; j++) { if (vals[j] < d->lo) d->lo = vals[j]; if (vals[j] > d->hi) d->hi = vals[j]; }
    d->next = ir_data_objects; ir_data_objects = d;
    ir_data_decl[ir_data_n] = g; ir_data_obj[ir_data_n] = d; ir_data_n++;
    return d;
}
static Decl *ir_find_global_const(LowerCtx *c, Id *name) {
    if (!name) return NULL;
    for (DeclList *d = c->globals; d; d = d->next) {
        Decl *dc = d->decl;
        if (!dc || dc->kind != DECL_VARIABLE) continue;
        if (!dc->as.variable_decl.init || dc->as.variable_decl.is_mutable) continue;
        Id *dn = dc->as.variable_decl.name;
        if (!dn) continue;
        if (dn->length == name->length &&
            strncmp(dn->name, name->name, (size_t)name->length) == 0)
            return dc;                                   // bare match
        size_t ml;                                       // `<mod>_<name>` match
        if (ir_mod_prefix(dc->defining_module, name, &ml) &&
            (size_t)name->length == ml + 1 + (size_t)dn->length &&
            strncmp(name->name + ml + 1, dn->name, (size_t)dn->length) == 0)
            return dc;
    }
    return NULL;
}
// "aggregate" here = decays to an element-base pointer read directly from its slot
// (a fixed array). A SLICE and a STRUCT are first-class values living in a normal
// slot (alloca + load/store by value); their fields/elements are reached by GEP.
static bool ir_type_is_agg(IrType *t) {
    return t && t->kind==IRT_ARRAY;
}

// A module-level struct declaration by name. Sema QUALIFIES a cross-module reference as
// `<defining_module>_<Name>` while the declaration keeps its bare name, so a bare-only match
// silently failed for every imported type — `std/fs.ln`'s `File` lowered to an OPAQUE struct
// with no fields. That is not a cosmetic loss: field access on it cannot resolve (the largest
// single cause of `incomplete`), and every field-sensitive analysis is blinded (it is what
// exempted std handles from leak checking until the pass was taught to fail closed on an
// opaque linear struct). ir_find_global_const already had to learn this; so does this.
static Decl *ir_find_struct_decl(LowerCtx *c, Id *name) {
    if (!name) return NULL;
    for (DeclList *d = c->globals; d; d = d->next) {
        Decl *dc = d->decl;
        if (!dc || dc->kind != DECL_STRUCT) continue;
        Id *dn = dc->as.struct_decl.name;
        if (!dn) continue;
        if (dn->length == name->length &&
            strncmp(dn->name, name->name, (size_t)name->length) == 0) return dc;  // bare
        size_t ml;                                                                // `<mod>_<Name>`
        if (ir_mod_prefix(dc->defining_module, name, &ml) &&
            (size_t)name->length == ml + 1 + (size_t)dn->length &&
            strncmp(name->name + ml + 1, dn->name, (size_t)dn->length) == 0)
            return dc;
    }
    return NULL;
}

// A module-level ENUM declaration, matched the same two ways as a struct.
static Decl *ir_find_enum_decl(LowerCtx *c, Id *name) {
    if (!name) return NULL;
    for (DeclList *d = c->globals; d; d = d->next) {
        Decl *dc = d->decl;
        if (!dc || dc->kind != DECL_ENUM) continue;
        Id *dn = dc->as.enum_decl.type_name;
        if (!dn) continue;
        if (dn->length == name->length &&
            strncmp(dn->name, name->name, (size_t)name->length) == 0) return dc;
        size_t ml;
        if (ir_mod_prefix(dc->defining_module, name, &ml) &&
            (size_t)name->length == ml + 1 + (size_t)dn->length &&
            strncmp(name->name + ml + 1, dn->name, (size_t)dn->length) == 0)
            return dc;
    }
    return NULL;
}
// Index of the variant named `vn` in `ed`, or -1. `suffix` also accepts a MANGLED
// reference (`<mod>_<Enum>_<Variant>`), which is how resolve.h rewrites a bare variant.
// ★ EXACT NAMES FIRST, AND A SUFFIX ONLY IF IT IS UNIQUE (I.105). One loop tried both, so in
// `{ Red, Dark_Red }` the name `Dark_Red` (and `<mod>_Shade_Dark_Red`) ended in `_Red` and took
// Red's arm: `case s { Dark_Red: … Red: … }` dispatched Red to the Dark_Red arm, in C and in the
// interpreter alike. A bare reference carries its variant (ir_variant_name_of), so the suffix is a
// fallback; two candidates there is a front-end defect, and it stops the build rather than guess.
static Id *ir_variant_name_of(Expr *e);
static int ir_variant_index(Decl *ed, Id *vn, bool suffix) {
    if (!ed || !vn) return -1;
    int k = 0;
    for (Variant *v = ed->as.enum_decl.variants; v; v = v->next, k++)
        if (v->name && v->name->length == vn->length &&
            strncmp(v->name->name, vn->name, (size_t)vn->length) == 0) return k;
    if (!suffix) return -1;
    int hit = -1, hits = 0; k = 0;
    for (Variant *v = ed->as.enum_decl.variants; v; v = v->next, k++) {
        if (!v->name || vn->length <= v->name->length) continue;
        const char *tail = vn->name + (vn->length - v->name->length);
        if (tail[-1] == '_' && strncmp(tail, v->name->name, (size_t)v->name->length) == 0) { hit = k; hits++; }
    }
    if (hits > 1) {
        fprintf(stderr, "internal error: the variant reference '%.*s' matches %d variants of "
                "'%.*s' by name; please report it.\n", (int)vn->length, vn->name, hits,
                (int)ed->as.enum_decl.type_name->length, ed->as.enum_decl.type_name->name);
        exit(1);
    }
    return hit;
}

// ── NESTED VARIANT PATTERNS ──────────────────────────────────────────────────────────────
// `case r { Err(NotFound): … Err(Permission): … }` names variants of the payload's OWN sum
// type: those are TESTS, while `Err(e)` binds. Matching only the outer variant made every
// `Err(...)` arm take the first one — `Err(Permission)` printed NotFound, a silently wrong
// dispatch rather than a build failure. Telling the two apart is a lookup, not a syntax rule,
// so it works at whatever depth the payload's type happens to be an enum.
static int ir_nested_variant_index(LowerCtx *c, IrType *fty, Expr *sub) {
    if (!sub || sub->kind!=EXPR_IDENTIFIER) return -1;
    if (!fty || fty->kind!=IRT_SUM || !fty->sname) return -1;
    Id tn; tn.name = fty->sname->name; tn.length = fty->sname->length;
    Decl *ed = ir_find_enum_decl(c, &tn);
    if (!ed) return -1;
    return ir_variant_index(ed, ir_variant_name_of(sub), true);
}
// The payload type of variant `k`'s field `j` (payloads are wrapped in a struct).
static IrType *ir_variant_field_type(IrType *plk, int j) {
    if (!plk) return NULL;
    return (plk->kind==IRT_STRUCT && j < plk->n_fields) ? plk->fields[j] : plk;
}
// ── STRING PATTERNS (spec 15) ────────────────────────────────────────────────────────────────
// `case s { "ab": … }` matches when s has the pattern's length and bytes. It lowers to the tests
// the spec's note describes: the length, then each byte, every failed test a branch to the next
// pattern. Each byte is read under `len == n`, so its bound is DISCHARGED by that guard rather
// than assumed; a fixed array whose length is not the pattern's never matches it. A string
// scrutinee had no lowering at all, and the whole `case` statement was dropped.
typedef struct { IrValue *data, *len; int64_t fixed; } IrStrScrut;   // fixed >= 0: an array
static bool ir_is_u8_string_type(IrType *t) {
    IrType *e = t ? t->elem : NULL;
    return t && (t->kind == IRT_SLICE || t->kind == IRT_ARRAY) &&
           e && e->kind == IRT_INT && e->bits == 8 && !e->is_signed;
}
// The scrutinee's bytes and length, computed once where it is evaluated, before any test.
static void ir_string_scrutinee(LowerCtx *c, IrValue *v, IrType *t, IrStrScrut *ss) {
    if (t->kind == IRT_SLICE) {
        ss->data = ir_slice_data(c->f, c->cur, v, t->elem);
        ss->len = ir_slice_len(c->f, c->cur, v); ss->fixed = -1;
    } else { ss->data = v; ss->len = NULL; ss->fixed = t->array_len; }
}
static void ir_match_string(LowerCtx *c, IrStrScrut *ss, Expr *pe, IrBlock *body, IrBlock *nxt) {
    int64_t n = pe->as.string_expr.length;
    IrType *usz = ir_type_int(c->a, 64, false), *u8 = ir_type_int(c->a, 8, false);
    if (ss->fixed >= 0) {
        if (ss->fixed != n) { ir_set_br(c->cur, nxt); return; }
    } else {
        IrBlock *bytes = ir_new_block(c->f);
        ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, IR_CMP_EQ, ss->len, ir_const_int(c->f, c->cur, n, usz)),
                       bytes, nxt);
        c->cur = bytes;
    }
    for (int64_t k = 0; k < n; k++) {
        IrBlock *next = ir_new_block(c->f);
        IrValue *ep = ir_elem_ptr(c->f, c->cur, ss->data, ir_const_int(c->f, c->cur, k, usz), u8);
        IrValue *by = ir_load(c->f, c->cur, ep, u8);
        IrValue *lit = ir_const_int(c->f, c->cur, (unsigned char)pe->as.string_expr.value[k], u8);
        ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, IR_CMP_EQ, by, lit), next, nxt);
        c->cur = next;
    }
    ir_set_br(c->cur, body);
}
// Branch on the outer tag test `eq` into `body`, threading the pattern's nested variant tests
// in between. Each sub-test sits in its own block, entered only once the outer tag is known,
// so IR_SUM_PAYLOAD is never emitted off its own variant. `c->cur` is left where the caller
// had it, so the caller's `c->cur = nxt` still means what it did.
static void ir_match_branch(LowerCtx *c, IrType *sumty, int k, Expr *pe, IrValue *v,
                            IrValue *eq, IrBlock *body, IrBlock *nxt) {
    IrType *plk = (sumty && k < sumty->n_fields) ? sumty->fields[k] : NULL;
    int nsub = 0;
    if (pe && pe->kind==EXPR_CALL && plk) {
        int j=0; for (ExprList *aa=pe->as.call_expr.args; aa; aa=aa->next, j++)
            if (ir_nested_variant_index(c, ir_variant_field_type(plk,j), aa->expr) >= 0) nsub++;
    }
    if (!nsub) { ir_set_br_cond(c->cur, eq, body, nxt); return; }
    IrBlock *keep = c->cur, *sub = ir_new_block(c->f);
    ir_set_br_cond(c->cur, eq, sub, nxt);
    c->cur = sub;
    int seen = 0, j = 0;
    for (ExprList *aa=pe->as.call_expr.args; aa; aa=aa->next, j++) {
        IrType *fty = ir_variant_field_type(plk, j);
        int vk = ir_nested_variant_index(c, fty, aa->expr);
        if (vk < 0) continue;
        IrValue *pv2 = ir_sum_payload(c->f, c->cur, v, k, j, fty);
        IrValue *stg = ir_sum_tag(c->f, c->cur, pv2);
        IrValue *skc = ir_const_int(c->f, c->cur, vk, stg->type);
        IrValue *seq = ir_icmp(c->f, c->cur, IR_CMP_EQ, stg, skc);
        seen++;
        IrBlock *tgt = (seen==nsub) ? body : ir_new_block(c->f);
        ir_set_br_cond(c->cur, seq, tgt, nxt);
        c->cur = tgt;
    }
    c->cur = keep;
}

// The variant name an expression names: `Shape.Circle` (member) or a bare/mangled `NotFound`.
static Id *ir_variant_name_of(Expr *e) {
    if (!e) return NULL;
    if (e->kind == EXPR_MEMBER)     return e->as.member_expr.member;
    if (e->kind == EXPR_IDENTIFIER) return e->as.identifier_expr.variant ? e->as.identifier_expr.variant->name
                                                                         : e->as.identifier_expr.id;
    return NULL;
}

// A type alias `type Name = <base> [refinement…]` by (bare) name.
static Decl *ir_find_type_alias(LowerCtx *c, Id *name) {
    if (!name) return NULL;
    for (DeclList *d = c->globals; d; d = d->next) {
        Decl *dc = d->decl;
        if (!dc || dc->kind != DECL_TYPE_ALIAS) continue;
        Id *dn = dc->as.type_alias_decl.name;
        if (dn && dn->length == name->length &&
            strncmp(dn->name, name->name, (size_t)name->length) == 0) return dc;
    }
    return NULL;
}
// The alias's *runtime* base type — the leftmost leaf of the RHS (refinement
// constraints like `!= 0` are for the VRA, not the representation).
static IrType *ir_lower_type(LowerCtx *c, Type *t);              // fwd
static IrType *ir_lower_type_impl(LowerCtx *c, Type *t);        // fwd (wrapped for the linear bit)

// Does a mutable borrow (`var x`) of this type have to travel as an ADDRESS for writes to
// reach the caller's storage? Yes for types that are COPIED (struct, scalar); no for a
// slice/array/pointer, which already carries a shared data pointer.
static bool ir_mut_by_address(const IrType *t);   // fwd
// ── A BORROW IS A POINTER, AND THE TYPE MUST SAY SO ─────────────────────────────────────
// A `var T` PARAMETER of a copied type has always been lowered to a pointer (see the param
// loop). A `var T` RETURN was not: its IR type was T, and the fact that it returns a reference
// was recorded as IrFunc.ret_borrows — a boolean beside the type, read by analysis/borrow.h and
// by nothing else. The IR was then ill-typed, `ret %1 : *i32` from a function typed i32, and
// every client that trusts types rather than the flag got it wrong. The IR's own C backend
// emitted an int32_t return and the write through the returned reference never reached the
// owner; src/emit/ was right only because it works from the AST.
//
// Same rule, same predicate, applied wherever a BINDING takes a borrow: the return type, and a
// local bound to one. Deliberately NOT folded into ir_lower_type — that is called for field
// types, element types and casts, where `var` means something else or nothing.
static IrType *ir_lower_borrow_binding_type(LowerCtx *c, Type *t) {
    IrType *lt = ir_lower_type(c, t);
    if (!t || t->mode != MODE_MUTABLE) return lt;
    if (ir_mut_by_address(lt)) {
        // COPIED types travel as an address, so the borrow IS a pointer and the type says so.
        IrType *ptr = ir_type_new(c->a, IRT_PTR); ptr->elem = lt; ptr->ptr_mut = true;
        ptr->borrowed = true;
        return ptr;
    }
    // A slice, array or pointer already carries a data pointer, so a mutable borrow of one
    // travels AS ITSELF — there is no wrapper whose shape could record the fact. Mark the type
    // instead, on a FRESH copy: `ir_lower_type` may hand back a structure another site shares,
    // and a borrow flag that leaked into a field or element type would make every value of
    // that type look like a reference. This is the case `IrFunc.ret_borrows` was really
    // covering, and the reason the fact needs a bit of its own rather than `ptr_mut`.
    IrType *cp = ir_type_new(c->a, lt->kind);
    *cp = *lt;
    cp->borrowed = true;
    return cp;
}

// Does a SHARED borrow of this parameter travel as an ADDRESS?
//
// The mutable case (`ir_mut_by_address`) has always said yes for COPIED types, because writes
// have to reach the caller's storage. The shared case said NO, and passed the aggregate BY
// VALUE — which is a COPY, and P1 forbids hidden copies outright: a shared borrow that copies
// its argument is not a borrow. It also left nothing for the backend to annotate, since
// `nonnull` and `access(read_only, n, m)` describe POINTER parameters, which is how the gap
// first showed up (D-50: `nonnull` short by 52, `access` short by 38).
//
// Only aggregates. A scalar is genuinely cheaper in a register and a copy of one is not
// observable; a slice/array/pointer already carries its data pointer. And only a SHARED
// binding: `mov` transfers ownership and must keep passing the value, or the callee would be
// consuming storage it does not own.
//
// ★ STRUCTS ONLY, AND THE REASON IS THE IR'S OWN DESIGN. A SUM's layout is deliberately NOT
// recorded here (chapter 0 §3: recording the niche would make a sum indistinguishable from a
// pointer with an odd range, and destroy the discrimination every analysis depends on) — so
// this rule cannot tell a tag-plus-payload aggregate from a NICHE-PACKED one. `*u8 | none` is
// exactly one pointer at run time, and passing its ADDRESS is strictly worse than passing it:
// an extra indirection for a word-sized value, plus a narrowing (`if x { use_it(x) }`) that
// now has to load before it can narrow. Measured: it broke that program's build.
//
// A struct is always a genuine aggregate, so the question does not arise for it. Sums wait for
// the LAYOUT PASS BELOW THE IR (plan item 2.2) — the same pass that has to exist before a
// second backend can agree with the first about niches. This is the first time that missing
// pass has blocked something concrete rather than being an argument.
static bool ir_shared_agg_by_address(const IrType *lt, const Type *declared) {
    if (!lt || !declared) return false;
    if (declared->mode != MODE_SHARED) return false;       // mov / var are the other rules
    return lt->kind == IRT_STRUCT;
}

static bool ir_mut_by_address(const IrType *t) {
    if (!t) return false;
    switch (t->kind) {
        // IRT_SUM belongs here and was missing. A sum is a COPIED aggregate exactly like a
        // struct — tag plus payload, or a niche-packed value — so a `var` borrow of one has to
        // travel as an address or the callee writes a local copy and the owner never sees it.
        // `proc mutate(var s Sh) { case s { Circle(r): s = Sh.Circle(r+1) ... } }` did exactly
        // that: the arm wrote the whole value back through what it thought was a reference,
        // the caller's `c` was unchanged, and the program returned 1 instead of 0
        // (`emit_gate`'s `adt_match_param_pass`). The old emitter was right because it works
        // from the AST, where the mode is still written down.
        //
        // The list is "types that are COPIED", and a sum was omitted because the predicate was
        // written when sums were not yet modelled — the same omission shape as the fixed-array
        // decay and the returned borrow: a case that simply was not there when the rule was.
        case IRT_SUM:
        case IRT_STRUCT: case IRT_INT: case IRT_BOOL: case IRT_FLOAT: return true;
        default: return false;   // slice / array / ptr / unit — already reference-like
    }
}
static bool ir_name_int(const char *nm, int len, int *bits, bool *sgn);  // fwd
static IrType *ir_resolve_alias_base(LowerCtx *c, Decl *ad) {
    Expr *rhs = ad->as.type_alias_decl.expr;
    while (rhs && rhs->kind == EXPR_BINARY) rhs = rhs->as.binary_expr.left;  // leftmost leaf
    if (!rhs) return NULL;
    if (rhs->kind == EXPR_TYPE && rhs->as.type_expr.type_value)   // sema-resolved base type
        return ir_lower_type(c, rhs->as.type_expr.type_value);
    if (rhs->kind == EXPR_IDENTIFIER) {
        Id *nm = rhs->as.identifier_expr.id; int b; bool s;
        if (nm->length==4 && strncmp(nm->name,"bool",4)==0) return ir_type_bool(c->a);
        if (ir_name_int(nm->name, (int)nm->length, &b, &s)) return ir_type_int(c->a, b, s);
    }
    if (rhs->type) return ir_lower_type(c, rhs->type);           // any other resolved expr
    return NULL;
}
// B4: copy `base` and narrow it by the alias's literal relational constraints. Only literal
// RHS bounds are type-level: a constraint against another identifier is per-VALUE and stays
// in the assume channel where it belongs.
// The refinement clauses of an alias (`type Small = u8 < 200`) and of a struct FIELD
// (`v i32 >= 0 and <= 3`) are the same list in the same shape — both are filed on the DECL by
// sema, not on the type expression. Only the alias case was read, so a refined field lowered
// to a bare `i32` and `a[b.v]` could not be proven in bounds even though the old engine proves
// it from the same declaration. Taking the LIST rather than the Decl lets both callers in.
static IrType *ir_refine_int_type_from(LowerCtx *c, IrType *base, ExprList *constraints) {
    int64_t lo, hi;
    if (!constraints) return base;
    if (!irtype_int_range(base, &lo, &hi)) return base;
    int64_t nlo = lo, nhi = hi; bool got = false;
    bool ne = false; int64_t nek = 0;
    for (ExprList *cn = constraints; cn; cn = cn->next) {
        Expr *e = cn->expr;
        {
            if (!e || e->kind != EXPR_BINARY) continue;
            Expr *r = e->as.binary_expr.right;
            if (!r || r->kind != EXPR_LITERAL) continue;
            int64_t k = (int64_t)r->as.literal_expr.value;
            switch (e->as.binary_expr.op) {
                case TOKEN_ANGLE_BRACKET_LEFT:        if (k-1 < nhi) { nhi = k-1; got = true; } break;
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  if (k   < nhi) { nhi = k;   got = true; } break;
                case TOKEN_ANGLE_BRACKET_RIGHT:       if (k+1 > nlo) { nlo = k+1; got = true; } break;
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: if (k   > nlo) { nlo = k;   got = true; } break;
                // `type Zero = i32 == 0` is the interval [0,0]. It was skipped, so the alias
                // constrained nothing and `var z Zero = 1` was refused only by the front end's
                // legacy check.
                case TOKEN_EQUAL_EQUAL:
                    if (k > nlo) nlo = k;
                    if (k < nhi) nhi = k;
                    got = true; break;
                case TOKEN_BANG_EQUAL:
                    ne = true; nek = k; got = true; break;
                default: break;
            }
        }
    }
    if (!got || nlo > nhi) return base;
    IrType *r = ir_type_new(c->a, IRT_INT);
    *r = *base;
    r->has_refine = true; r->refine_lo = nlo; r->refine_hi = nhi;
    if (ne) { r->has_ne = true; r->refine_ne = nek; }
    return r;
}

static IrType *ir_refine_int_type(LowerCtx *c, IrType *base, Decl *ad) {
    if (!ad || ad->kind != DECL_TYPE_ALIAS) return base;
    return ir_refine_int_type_from(c, base, ad->as.type_alias_decl.constraints);
}

// Index of field `m` in an IRT_STRUCT (uses the IR type's own field table — no AST).
static int ir_field_index(IrType *st, Id *m, IrType **fty) {
    if (!st || st->kind != IRT_STRUCT || !m) return -1;
    for (int i = 0; i < st->n_fields; i++) {
        IrName *fn = st->field_names[i];   // IR-owned; compare against the AST member id
        if (fn && fn->length == m->length &&
            strncmp(fn->name, m->name, (size_t)m->length) == 0) {
            if (fty) *fty = st->fields[i];
            return i;
        }
    }
    return -1;
}

// ── type bridge: AST Type → IrType (core cases) ──────────────────────────────
static bool ir_name_int(const char *nm, int len, int *bits, bool *sgn) {
    if (len >= 2 && (nm[0]=='i'||nm[0]=='u')) {
        int b=0; bool ok=true;
        for (int k=1;k<len;k++){ if(nm[k]<'0'||nm[k]>'9'){ok=false;break;} b=b*10+(nm[k]-'0'); }
        if (ok && b>=1 && b<=64){ *bits=b; *sgn=(nm[0]=='i'); return true; }
    }
    struct { const char*n; int b; bool s; } al[] = {
        {"usize",64,false},{"isize",64,true},{"int",32,true},{NULL,0,false} };
    for (int i=0; al[i].n; i++)
        if ((int)strlen(al[i].n)==len && strncmp(nm,al[i].n,len)==0){ *bits=al[i].b; *sgn=al[i].s; return true; }
    return false;
}
// B5: tag the lowered type with the linearity qualifier (sema_type_is_linear is the
// front-end's linearity oracle; the fact is carried on the IR type so the linearity pass
// reads it WITHOUT the AST). Wrapper over the real lowering so every return path is tagged.
static IrType *ir_lower_type(LowerCtx *c, Type *t) {
    IrType *r = ir_lower_type_impl(c, t);
    if (r && t && sema_type_is_linear(t)) r->linear = true;
    return r;
}
static IrType *ir_lower_type_impl(LowerCtx *c, Type *t) {
    if (!t) return ir_type_new(c->a, IRT_UNIT);
    // `T | m1 | m2` — an error union. It had NO case here at all, so it silently lowered to
    // its payload type T and every marker vanished: `try` and `else` had nothing to test and
    // became opaque placeholders, which is the whole `errors/` family. It is a SUM: variant 0
    // carries T, one empty variant per marker. The LAYOUT — a tagged pair, or the old
    // backend's niche where markers are out-of-range values of T — is deliberately not
    // recorded, exactly as for a named enum: that is the backend's choice, not the IR's.
    if (t->kind == TYPE_FUNC) {
        IrType *ft = ir_type_new(c->a, IRT_FUNC);
        ft->fn_row = t->func_effects;            // the arrow's effect bound, as a row
        ft->elem = t->element_type ? ir_lower_type(c, t->element_type) : NULL;
        int n = 0; for (TypeList *p = t->func_params; p; p = p->next) n++;
        ft->n_fields = n;
        ft->fields = arena_push_many_aligned(c->a, IrType*, n > 0 ? n : 1);
        int k = 0; for (TypeList *p = t->func_params; p; p = p->next, k++)
            ft->fields[k] = ir_lower_type(c, p->type);
        return ft;
    }
    if (t->kind == TYPE_VECTOR) {
        IrType *v = ir_type_new(c->a, IRT_VECTOR);
        v->elem = ir_lower_type(c, t->element_type);
        v->array_len = t->array_len;                 // the lane count
        return v;
    }
    if (t->kind == TYPE_UNION) {
        // Sema already lowers a union to a real, NAMED enum (`__U_<T>_<m1>_<m2>`, one payload
        // variant plus one empty variant per marker) and rewrites most references to it — but
        // not all: a call site could still carry the raw TYPE_UNION, and with no case here it
        // fell through to the payload type, so `try`/`else` had nothing to test.
        //
        // Find sema's enum rather than synthesising a second type for the same union: two
        // structurally-identical-but-distinct IrTypes is worse than none, because the call's
        // result then does not match the callee's declared return.
        // ★ BY SEMA'S OWN NAME, WHICH INCLUDES THE VALUE TYPE (I.106). This matched the MARKER
        // NAMES alone, so `*u8 | NotFound` and `Small | NotFound` were one union here: a call to
        // the second stored its byte in the first's pointer, and gcc refused the C.
        char un[256]; union_mangled_name(t, un, sizeof un);
        size_t ul = strlen(un);
        for (DeclList *d = c->globals; d; d = d->next) {
            Decl *ed = d->decl;
            if (!ed || ed->kind != DECL_ENUM || !ed->as.enum_decl.is_union) continue;
            Id *en = ed->as.enum_decl.type_name;
            if (!en || (size_t)en->length != ul || strncmp(en->name, un, ul) != 0) continue;
            Type tt; memset(&tt, 0, sizeof tt); tt.kind = TYPE_SIMPLE; tt.base_type = en;
            return ir_lower_type(c, &tt);              // the ENUM path: cached, named, shared
        }
        return ir_lower_type(c, t->element_type);      // no enum found: the payload alone
    }
    switch (t->kind) {
        case TYPE_SIMPLE: {
            if (t->base_type) {
                int len=(int)t->base_type->length; const char *nm=t->base_type->name;
                if (len==4 && strncmp(nm,"bool",4)==0) return ir_type_bool(c->a);
                // f32/f64 were not recognised at all, so a float local's slot lowered to an
                // opaque pointer and every float value in the program was lost.
                if (len==3 && nm[0]=='f' && nm[1]=='3' && nm[2]=='2') return ir_type_float(c->a,32);
                if (len==3 && nm[0]=='f' && nm[1]=='6' && nm[2]=='4') return ir_type_float(c->a,64);
                // `float` is the third spelling and was missing, so a `var f float` slot
                // lowered to an opaque pointer and gcc refused the assignment outright.
                // Mapped to f32 to MATCH THE EMITTED C the corpus runs against — note the
                // front end disagrees with itself here (typecheck.h calls `float` an alias of
                // f64 while the old backend emits C `float`); recorded rather than silently
                // picked, because the two give different results for a large value.
                if (len==5 && strncmp(nm,"float",5)==0) return ir_type_float(c->a,32);
                int b; bool s;
                if (t->int_width_cache>0) return ir_type_int(c->a, t->int_width_cache, t->int_signed_cache);
                if (ir_name_int(nm,len,&b,&s)) return ir_type_int(c->a, b, s);
            }
            // a type alias → its runtime base type (refinements are the VRA's concern)
            Decl *ad = ir_find_type_alias(c, t->base_type);
            if (ad && c->const_depth < 32) {
                c->const_depth++;
                IrType *r = ir_resolve_alias_base(c, ad);
                c->const_depth--;
                // B4: the alias's CONSTRAINT is a property of the type — carry it, do not
                // discard it. `type Small = u8 < 200` was resolving to a bare `u8`, so a
                // value of type Small was indistinguishable from any other u8.
                if (r && r->kind == IRT_INT) r = ir_refine_int_type(c, r, ad);
                if (r) return r;
            }
            // a named ENUM → a self-contained IRT_SUM (variant table). Payload-carrying
            // variants get an IRT_STRUCT payload; payload-less ones get NULL. The LAYOUT
            // (tag+union vs niche) is deliberately not recorded — that is the backend's.
            Decl *ed = ir_find_enum_decl(c, t->base_type);
            if (ed) {
                for (int i=0;i<c->scache_n;i++) if (c->scache_decl[i]==ed) return c->scache_type[i];
                IrType *r = ir_type_new(c->a, IRT_SUM);
                Id *enm = ed->as.enum_decl.type_name;
                if (enm) r->sname = ir_intern(c->a, enm->name, enm->length);
                r->sum_backing_bits = ed->as.enum_decl.backing_bits;   // the parser checked it
                r->ordered_decl = ed->as.enum_decl.is_ordered;          // [ordered]: every payload
                if (ed->as.enum_decl.is_union) {     // `T | markers`: the payload is variant 0
                    bool bare = true;
                    for (Variant *v = ed->as.enum_decl.variants ? ed->as.enum_decl.variants->next : NULL;
                         v; v = v->next) if (v->fields) bare = false;
                    r->sum_niche_mandatory = bare;
                }
                if (c->scache_n < 64) { c->scache_decl[c->scache_n]=ed;
                                        c->scache_type[c->scache_n]=r; c->scache_n++; }
                int nv=0; for (Variant *v = ed->as.enum_decl.variants; v; v=v->next) nv++;
                r->n_fields    = nv;
                r->fields      = arena_push_many_aligned(c->a, IrType*, nv>0?nv:1);
                r->field_names = arena_push_many_aligned(c->a, IrName*, nv>0?nv:1);
                int i=0;
                for (Variant *v = ed->as.enum_decl.variants; v; v=v->next, i++) {
                    r->field_names[i] = v->name ? ir_intern(c->a, v->name->name, v->name->length) : NULL;
                    int nf=0; for (DeclList *fl=v->fields; fl; fl=fl->next)
                        if (fl->decl && fl->decl->kind==DECL_VARIABLE) nf++;
                    if (nf == 0) { r->fields[i] = NULL; continue; }   // a payload-less variant
                    IrType *pt = ir_type_new(c->a, IRT_STRUCT);
                    pt->sname = r->field_names[i];
                    pt->n_fields = nf;
                    pt->fields      = arena_push_many_aligned(c->a, IrType*, nf);
                    pt->field_names = arena_push_many_aligned(c->a, IrName*, nf);
                    int j=0; for (DeclList *fl=v->fields; fl; fl=fl->next) {
                        if (!fl->decl || fl->decl->kind!=DECL_VARIABLE) continue;
                        // A variant field's refinement is its type's, exactly as a struct field's
                        // is (below): the payload's narrowing at construction is checked into it,
                        // and a read of the payload may assume it (I.66).
                        pt->fields[j] = ir_refine_int_type_from(c,
                                            ir_lower_type(c, fl->decl->as.variable_decl.type),
                                            fl->decl->as.variable_decl.constraints);
                        Id *fn = fl->decl->as.variable_decl.name;
                        pt->field_names[j] = fn ? ir_intern(c->a, fn->name, fn->length) : NULL;
                        j++;
                    }
                    r->fields[i] = pt;
                }
                return r;
            }
            // a named struct → a self-contained IRT_STRUCT (lowered field table)
            Decl *sd = ir_find_struct_decl(c, t->base_type);
            if (sd) {
                for (int i=0;i<c->scache_n;i++) if (c->scache_decl[i]==sd) return c->scache_type[i];
                IrType *r = ir_type_new(c->a, IRT_STRUCT);
                Id *snm = sd->as.struct_decl.name;                    // intern the struct name
                if (snm) r->sname = ir_intern(c->a, snm->name, snm->length);
                r->packed_decl = sd->as.struct_decl.is_packed;       // layout.h decides the rest
                r->ordered_decl = sd->as.struct_decl.is_ordered;
                if (c->scache_n < 64) { c->scache_decl[c->scache_n]=sd;
                                        c->scache_type[c->scache_n]=r; c->scache_n++; }
                int nf=0; for (DeclList *fl=sd->as.struct_decl.fields; fl; fl=fl->next)
                    if (fl->decl && fl->decl->kind==DECL_VARIABLE) nf++;
                r->n_fields = nf;
                r->fields       = arena_push_many_aligned(c->a, IrType*, nf>0?nf:1);
                r->field_names  = arena_push_many_aligned(c->a, IrName*, nf>0?nf:1);
                int i=0; for (DeclList *fl=sd->as.struct_decl.fields; fl; fl=fl->next) {
                    if (!fl->decl || fl->decl->kind!=DECL_VARIABLE) continue;
                    r->fields[i]      = ir_refine_int_type_from(c,
                                            ir_lower_type(c, fl->decl->as.variable_decl.type),
                                            fl->decl->as.variable_decl.constraints);
                    Id *fnm = fl->decl->as.variable_decl.name;
                    r->field_names[i] = fnm ? ir_intern(c->a, fnm->name, fnm->length) : NULL;
                    i++;
                }
                return r;
            }
            return ir_type_new(c->a, IRT_STRUCT);   // unresolved named type — opaque
        }
        case TYPE_ARRAY: {
            IrType *el = ir_lower_type(c, t->element_type);
            if (t->array_len >= 0) { IrType *r=ir_type_new(c->a,IRT_ARRAY); r->elem=el; r->array_len=t->array_len; return r; }
            IrType *r=ir_type_new(c->a,IRT_SLICE); r->elem=el; return r;   // dynamic → slice
        }
        case TYPE_SLICE: { IrType *r=ir_type_new(c->a,IRT_SLICE); r->elem=ir_lower_type(c,t->element_type); return r; }
        case TYPE_POINTER:{ IrType *r=ir_type_new(c->a,IRT_PTR); r->elem=ir_lower_type(c,t->element_type); r->ptr_mut=t->pointee_mutable; r->is_raw=true; return r; }   // `*T` is unsafe: no exclusivity to promise
        default: return ir_type_new(c->a, IRT_UNIT);
    }
}

// forward
// ★ AN IMMUTABLE BINDING'S STORAGE IS IMMUTABLE BY ANY PATH. sema refuses a write that NAMES it
// (`a[0] = 9`, E009), but a slice of it kept in a `var` binding or handed back by a call was
// written freely: `var s = xs[0..2]  s[0] = 9` changed `xs`. What lowering emits for the binding
// is its initialisation; everything it emitted is stamped with one seal (ir.h), and a write
// that reaches the storage from anywhere else is refused (borrow.h) and, at run time, a proof
// failure in the interpreter. A module constant's per-use copy is sealed the same way.
static int32_t ir_seal_next = 0;
typedef struct { IrBlock *b0; IrInstr *t0; IrBlock *last; } IrSealMark;
static IrSealMark ir_seal_open(LowerCtx *c) {
    IrSealMark m = { c->cur, c->cur ? c->cur->instrs_tail : NULL, c->f->blocks_tail };
    return m;
}
static void ir_seal_stamp(IrInstr *i, int32_t id) {
    if (!i->seal && (i->op == IR_ALLOCA || i->op == IR_STORE || i->op == IR_CALL)) i->seal = id;
}
static void ir_seal_close(LowerCtx *c, IrSealMark m) {        // an inner seal keeps its own id
    int32_t id = ++ir_seal_next;
    if (m.b0) for (IrInstr *i = m.t0 ? m.t0->next : m.b0->instrs; i; i = i->next) ir_seal_stamp(i, id);
    for (IrBlock *b = m.last ? m.last->next : c->f->blocks; b; b = b->next)
        if (b != m.b0) for (IrInstr *i = b->instrs; i; i = i->next) ir_seal_stamp(i, id);
}
static IrValue *ir_lower_expr(LowerCtx *c, Expr *e);
// Coerce a value into the representation a DECLARED slot asks for. Two coercions the
// language performs implicitly and the IR must make explicit:
//   slice → pointer   a string literal is a `u8[:0]`; a `*u8` field wants its DATA pointer,
//                     and storing the two-word slice there is wrong at the ABI, not merely
//                     ill-typed in the emitted C.
//   array → slice     a fixed array decays where a slice is expected.
// The call path grew both of these one at a time; naming them once is what lets STRUCT and
// VARIANT construction have them too — `OptionByte.Some("hi")` needed exactly this.
//
// ★ ONE relation, applied at EVERY boundary: call argument, struct and variant construction,
// typed declaration, assignment, return. Until 2026-09-28 the call path kept its own copy (the
// only complete one) and struct construction, declaration, assignment and return had none —
// `W(buf, 7)` built `(W){ buf, 7 }` and C's brace elision put 7 in the slice's LENGTH.
// An array literal of exactly N elements, built as the N-lane vector `vt` (NULL otherwise).
static IrValue *ir_lower_vec_literal(LowerCtx *c, Expr *lit, IrType *vt) {
    int n = 0; for (ExprList *el = lit->as.array_literal_expr.elements; el; el = el->next) n++;
    if (n <= 0 || n != (int)vt->array_len) return NULL;
    IrValue **lanes = arena_push_many_aligned(c->a, IrValue*, n);
    int k = 0;
    for (ExprList *el = lit->as.array_literal_expr.elements; el; el = el->next, k++)
        lanes[k] = ir_lower_expr(c, el->expr);
    return ir_struct_new(c->f, c->cur, vt, lanes, n);
}
static IrValue *ir_coerce_repr(LowerCtx *c, IrValue *v, IrType *want, Expr *src) {
    if (!v || !v->type || !want) return v;
    // A vector comparison's MASK has signed lanes of the operands' width (ir_icmp: the ISA's
    // convention); Lain types it as the operands' vector. Where the two met — `var m u8x16 =
    // a == 7`, an argument, a field, a return — the C was `Vec_16_u8 = Vec_16_i8`, which gcc
    // refuses. Same shape, integer lanes, different signedness: a bit-cast.
    if (want->kind==IRT_VECTOR && v->type->kind==IRT_VECTOR && want->array_len==v->type->array_len &&
        want->elem && v->type->elem && want->elem->kind==IRT_INT && v->type->elem->kind==IRT_INT &&
        want->elem->bits==v->type->elem->bits && want->elem->is_signed!=v->type->elem->is_signed) {
        IrInstr *cv = ir_instr(c->f, IR_CAST, want, 1);
        cv->operands[0] = v; cv->aux.cast_kind = IR_CAST_BITCAST;
        ir_emit(c->cur, cv);
        return cv->result;
    }
    // An ARRAY LITERAL where a VECTOR is wanted — `Pair([5, 6, 7, 8], 9)` into a u32x4 field.
    // The literal types as an array and was materialised as one, so the field received the
    // array's BASE POINTER: garbage lanes under gcc, and C clang refuses. Only a declaration
    // built the vector (from its declared type). Here the lanes are read back out of the
    // materialised array, so no element expression is evaluated twice.
    if (want->kind==IRT_VECTOR && src && src->kind==EXPR_ARRAY_LITERAL &&
        (v->type->kind==IRT_PTR || v->type->kind==IRT_ARRAY) && v->type->elem) {
        int n = 0; for (ExprList *el = src->as.array_literal_expr.elements; el; el = el->next) n++;
        if (n == (int)want->array_len && n > 0) {
            IrValue **lanes = arena_push_many_aligned(c->a, IrValue*, n);
            for (int k = 0; k < n; k++) {
                IrValue *ix = ir_const_int(c->f, c->cur, k, ir_type_int(c->a,64,false));
                IrValue *p  = ir_elem_ptr(c->f, c->cur, v, ix, v->type->elem);
                lanes[k] = ir_load(c->f, c->cur, p, v->type->elem);
            }
            return ir_struct_new(c->f, c->cur, want, lanes, n);
        }
    }
    if (want->kind==IRT_PTR && v->type->kind==IRT_SLICE)
        return ir_slice_data(c->f, c->cur, v, want->elem ? want->elem : v->type->elem);
    // A decayed array's base points at an ELEMENT; a pointer to a whole slice or struct (a `var`
    // parameter's value) is not an array and must not be rewrapped as a zero-length one. The
    // SOURCE's type says which: an array of structs decays to a pointer to a struct too.
    bool src_arr = src && src->type && src->type->kind == TYPE_ARRAY;
    bool elem_ptr = v->type->kind==IRT_PTR && (src_arr ||
                    !(v->type->elem && (v->type->elem->kind==IRT_SLICE || v->type->elem->kind==IRT_STRUCT)));
    if (want->kind==IRT_SLICE && (elem_ptr || v->type->kind==IRT_ARRAY)) {
        IrValue *data = v;
        int64_t ne;
        if (v->type->kind==IRT_ARRAY) {
            // ★ A FIXED-ARRAY PARAMETER IS AN ARRAY VALUE, not a pointer, and its length is IN
            // the type — better than the pointer case's reading of the AST.
            ne = v->type->array_len;
            IrValue *z = ir_const_int(c->f, c->cur, 0, ir_type_int(c->a,64,false));
            data = ir_elem_ptr(c->f, c->cur, v, z, v->type->elem);
        } else {
            ne = (src && src->type && src->type->kind==TYPE_ARRAY) ? src->type->array_len : 0;
        }
        IrValue *ln = ir_const_int(c->f, c->cur, ne, ir_type_int(c->a,64,false));
        return ir_make_slice(c->f, c->cur, data, ln, want->elem);
    }
    return v;
}


static void ir_lower_flush_defers(LowerCtx *c);   // fwd (try/else propagate through it)
static void     ir_lower_stmts(LowerCtx *c, StmtList *body);

// map an AST comparison token to an IrCmp given signedness. false ⇒ not a comparison.
static bool ir_tok_cmp(TokenKind op, bool sgn, IrCmp *out) {
    switch (op) {
        case TOKEN_ANGLE_BRACKET_LEFT:        *out = sgn?IR_CMP_SLT:IR_CMP_ULT; return true;
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  *out = sgn?IR_CMP_SLE:IR_CMP_ULE; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT:       *out = sgn?IR_CMP_SGT:IR_CMP_UGT; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: *out = sgn?IR_CMP_SGE:IR_CMP_UGE; return true;
        default: return false;
    }
}

// ── RELATIONAL FIELD INVARIANTS (`pos usize <= src.len`, `len usize <= cap`) ─────────────
// A field refinement naming ANOTHER field (resolve.h admits `f CMP g` and `f CMP g.len`). It
// is the `in` invariant generalised: a promise about the value for its whole life, so it is
// ASSERTED where it could stop holding — construction, and a write to EITHER field — and
// ASSUMED where it is read. A `var` reference to either field is refused in the front end,
// because a write through one is not a write this lowering sees.
typedef struct { IrCmp cmp; int other; bool is_len; } IrFieldRel;
static int ir_field_relations(LowerCtx *c, IrType *sty, int fidx, IrFieldRel *out, int max) {
    if (!sty || sty->kind!=IRT_STRUCT || fidx<0 || fidx>=sty->n_fields || !sty->field_names || !sty->sname) return 0;
    Id sn; sn.name = sty->sname->name; sn.length = sty->sname->length;
    Decl *sd = ir_find_struct_decl(c, &sn);
    if (!sd || sd->kind != DECL_STRUCT) return 0;
    int k = 0; Decl *fd = NULL;
    for (DeclList *fl = sd->as.struct_decl.fields; fl; fl = fl->next) {
        if (!fl->decl || fl->decl->kind != DECL_VARIABLE) continue;
        if (k == fidx) { fd = fl->decl; break; }
        k++;
    }
    if (!fd) return 0;
    IrType *ft = sty->fields[fidx];
    bool sgn = ft && ft->kind==IRT_INT && ft->is_signed;
    int n = 0;
    for (ExprList *cn = fd->as.variable_decl.constraints; cn && n < max; cn = cn->next) {
        Expr *con = cn->expr;
        if (!con || con->kind != EXPR_BINARY) continue;
        Expr *rhs = con->as.binary_expr.right;
        bool is_len = false;
        if (rhs && rhs->kind == EXPR_MEMBER) { is_len = true; rhs = rhs->as.member_expr.target; }
        if (!rhs || rhs->kind != EXPR_IDENTIFIER || !rhs->as.identifier_expr.id) continue;
        Id *gn = rhs->as.identifier_expr.id;
        int oj = -1;
        for (int q = 0; q < sty->n_fields; q++) {
            IrName *qn = sty->field_names[q];
            if (qn && qn->length == gn->length && strncmp(qn->name, gn->name, (size_t)gn->length) == 0) { oj = q; break; }
        }
        IrCmp cmp;
        if (oj < 0 || oj == fidx || !ir_tok_cmp(con->as.binary_expr.op, sgn, &cmp)) continue;
        out[n].cmp = cmp; out[n].other = oj; out[n].is_len = is_len; n++;
    }
    return n;
}
// ── A SLICE FIELD'S LENGTH BOUND: `src u8[<= 4294967295]` (DECIDE-P) ────────────────────────
// The grammar a parameter already uses for its length (`a i32[>= 2]`), on a field, where it is a
// promise for the value's whole life. It is ASSERTED where the length can change — construction
// and a store to the field — and ASSUMED where the field is read. Nothing else can change it: a
// `var` reference to a slice cannot rebind it (E009) and an element write keeps the length.
// It is the fact a narrow position needs: `pos u32 <= src.len` proves `pos + 1` only if src.len
// fits a u32, and a usize length says nothing of the kind (Marco's lexer, "max file ~4 GB").
static bool ir_field_len_bound(LowerCtx *c, IrType *sty, int fidx, IrCmp *cmp, int64_t *k) {
    if (!sty || sty->kind!=IRT_STRUCT || fidx<0 || fidx>=sty->n_fields || !sty->sname) return false;
    Id sn; sn.name = sty->sname->name; sn.length = sty->sname->length;
    Decl *sd = ir_find_struct_decl(c, &sn);
    if (!sd || sd->kind != DECL_STRUCT) return false;
    int q = 0; Decl *fd = NULL;
    for (DeclList *fl = sd->as.struct_decl.fields; fl; fl = fl->next) {
        if (!fl->decl || fl->decl->kind != DECL_VARIABLE) continue;
        if (q == fidx) { fd = fl->decl; break; }
        q++;
    }
    Type *t = fd ? fd->as.variable_decl.type : NULL;
    if (!t || t->kind != TYPE_ARRAY || t->array_len >= 0 || !t->size_expr ||
        t->size_expr->kind != EXPR_LITERAL) return false;
    if (t->size_relop == TOKEN_EQUAL_EQUAL) *cmp = IR_CMP_EQ;
    else if (!ir_tok_cmp(t->size_relop, false, cmp)) return false;
    *k = t->size_expr->as.literal_expr.value;
    return true;
}
static void ir_field_len_bound_fact(LowerCtx *c, IrType *sty, int fidx, IrValue *sv, bool assert_it) {
    IrCmp cmp; int64_t k;
    if (!sv || !sv->type || sv->type->kind != IRT_SLICE || !ir_field_len_bound(c, sty, fidx, &cmp, &k)) return;
    IrValue *len  = ir_slice_len(c->f, c->cur, sv);
    IrValue *cond = ir_icmp(c->f, c->cur, cmp, len, ir_const_int(c->f, c->cur, k, len->type));
    if (assert_it) ir_assert_coded(c->f, c->cur, cond, 121);
    else           ir_assume(c->f, c->cur, cond);
}
// The relation's right-hand side, from the OTHER field's value: that value itself, or its length.
static IrValue *ir_field_rel_rhs(LowerCtx *c, IrValue *ov, IrType *oty, bool is_len, IrType *ity) {
    if (!ov || !oty) return NULL;
    if (!is_len) return (oty->kind == IRT_INT) ? ov : NULL;
    if (oty->kind == IRT_SLICE && ov->type && ov->type->kind == IRT_SLICE) return ir_slice_len(c->f, c->cur, ov);
    if (oty->kind == IRT_ARRAY && oty->array_len >= 0)
        return ir_const_int(c->f, c->cur, oty->array_len, ity ? ity : ir_type_int(c->a,64,false));
    return NULL;
}
// ...read from memory: the other field of the struct at `base`.
static IrValue *ir_field_rel_rhs_at(LowerCtx *c, IrValue *base, IrType *sty, IrFieldRel *r, IrType *ity) {
    IrType *oty = sty->fields[r->other];
    if (!oty) return NULL;
    if (r->is_len && oty->kind == IRT_ARRAY)
        return ir_field_rel_rhs(c, base, oty, true, ity);          // a constant: no load needed
    IrValue *ov = ir_load(c->f, c->cur, ir_field_ptr(c->f, c->cur, base, r->other, oty), oty);
    return ir_field_rel_rhs(c, ov, oty, r->is_len, ity);
}

// B4-lite (dependent lengths): a slice/dynamic-array param whose type carries a length
// constraint `i32[m]` / `i32[>= n]` / `i32[out.len]` becomes an entry
// `assume(slice_len(p) relop <expr>)` — connecting the runtime length to the symbol.
// Run AFTER all params are in scope so the length expr (another param) resolves.
// Lower a refinement RHS robustly — the refinement/size exprs are NOT type-checked by
// sema (they come through untyped), so a literal gets the fallback type and `x.len` is
// resolved by the LOWERED value's IR kind, never the (missing) AST type.
static IrValue *ir_lower_refinement_rhs(LowerCtx *c, Expr *rhs, IrType *fallback_ty) {
    if (!rhs) return NULL;
    // (The literal special-case that used to live here was the workaround for D-13: refinement
    // expressions were never resolved OR type-checked, so the RHS of `n < 4096` arrived with
    // no type and would have lowered to a `unit`-typed constant. Sema now resolves and infers
    // them, and the bounds survey is IDENTICAL with and without the workaround — 802/815 both
    // ways — so it is dead and its removal is the statement that the defect is closed.)
    (void)fallback_ty;
    if (rhs->kind==EXPR_MEMBER && rhs->as.member_expr.member && rhs->as.member_expr.member->length==3
        && strncmp(rhs->as.member_expr.member->name,"len",3)==0) {
        // ★ `.len` of a FIXED array is its constant length. Only a slice's was resolved, so
        // `i usize < a.len` over `a u8[4096]` gave the callee no assume (and the call site no
        // assert — the precondition was silently a no-op) while `i usize < 4096` proved.
        Type *at = rhs->as.member_expr.target ? rhs->as.member_expr.target->type : NULL;
        while (at && at->kind == TYPE_COMPTIME) at = at->element_type;
        if (at && at->kind == TYPE_ARRAY && at->array_len >= 0)
            return ir_const_int(c->f, c->cur, at->array_len, ir_type_int(c->a,64,false));
        IrValue *tv = ir_lower_expr(c, rhs->as.member_expr.target);
        if (tv && tv->type && tv->type->kind==IRT_ARRAY && tv->type->array_len >= 0)
            return ir_const_int(c->f, c->cur, tv->type->array_len, ir_type_int(c->a,64,false));
        return (tv && tv->type && tv->type->kind==IRT_SLICE) ? ir_slice_len(c->f, c->cur, tv) : NULL;
    }
    return ir_lower_expr(c, rhs);
}

static void ir_lower_slice_len_refinement(LowerCtx *c, IrValue *pv, Type *pty) {
    if (!pv || !pty || pty->kind!=TYPE_ARRAY || pty->array_len>=0 || !pty->size_expr) return;
    IrValue *L  = ir_slice_len(c->f, c->cur, pv);
    IrValue *rv = ir_lower_refinement_rhs(c, pty->size_expr, ir_type_int(c->a,64,false));
    if (!rv || !rv->type || rv->type->kind!=IRT_INT) return;
    if (pty->size_relop == TOKEN_EQUAL_EQUAL) {           // len == expr
        ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_ULE, L, rv));
        ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_UGE, L, rv));
    } else { IrCmp cmp;
        if (ir_tok_cmp(pty->size_relop, false, &cmp))     // len relop expr (len is usize)
            ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, L, rv));
    }
}

// S2: a dependent length that is a PRODUCT of runtime extents — `a i32[h * w]` — declares a
// rank-N strided region, not merely a flat one of that size. Emit the shape so the bounds
// consumer can FACTOR `a[i*w + j]` into `i < h ∧ j < w` instead of facing the nonlinear
// `i*w + j < h*w`. Extents are collected outermost-first, so `h * w` gives (h, w) and
// row-major strides (w, 1).
//
// Only a product of plain identifiers is recognised — anything else stays a flat region,
// which is the conservative default (no shape ⇒ no factoring ⇒ the old behaviour).
static int ir_collect_extents(LowerCtx *c, Expr *e, IrValue **out, int cap) {
    if (!e || cap <= 0) return -1;
    if (e->kind == EXPR_BINARY && e->as.binary_expr.op == TOKEN_ASTERISK) {
        int n = ir_collect_extents(c, e->as.binary_expr.left, out, cap);
        if (n < 0) return -1;
        int m = ir_collect_extents(c, e->as.binary_expr.right, out+n, cap-n);
        if (m < 0) return -1;
        return n+m;
    }
    if (e->kind == EXPR_IDENTIFIER) {
        IrLocal *l = ir_env_find(c, e->as.identifier_expr.id);
        IrValue *v = l ? (l->param ? l->param : l->slot) : NULL;
        if (!v || !v->type || v->type->kind != IRT_INT) return -1;
        out[0] = v; return 1;
    }
    return -1;
}
static void ir_lower_region_shape(LowerCtx *c, IrValue *pv, Type *pty) {
    if (!pv || !pty || pty->kind!=TYPE_ARRAY || pty->array_len>=0 || !pty->size_expr) return;
    if (pty->size_relop != TOKEN_EQUAL_EQUAL && pty->size_relop != 0) return;  // len == expr only
    IrValue *ext[8];
    int rank = ir_collect_extents(c, pty->size_expr, ext, 8);
    if (rank < 2) return;                       // rank 1 is the ordinary flat region
    ir_shape(c->f, c->cur, pv, ext, rank);
}

// ★ A CONTRACT TERM THAT IS A CONSTANT (`K`, `K - 1`, `4 * K`, a module constant computed at
// compile time) has one value on both sides of a call. Both resolvers below knew a literal, a
// parameter and `p.len` only, while the callee's ENTRY side (ir_lower_param_refinements,
// ir_lower_slice_len_refinement) lowers any expression. So for `K u32 = 10`, `func g(n u32 < K)`
// ASSUMED n < 10 at its entry and no call asserted it: `g(50)` compiled, and so did
// `func g(a u8[K])` called with a 2-element array, which read past it (ASan: stack-buffer-
// overflow). Names resolve through the front end's declaration (sa_named_constant reads e->decl),
// so a parameter that shadows a module constant is not mistaken for it. The return side resolves
// through the same function on both ends (ir_contract_rhs_in_callee, ir_resolve_contract_rhs), so
// the two agree term for term.
static bool ir_contract_const(Expr *e, int64_t *v) {
    bool lay = false; __int128 x;
    if (!e || !sa_is_const(e, &lay) || lay || !sa_eval(e, &x)) return false;
    if (x < (__int128)INT64_MIN || x > (__int128)INT64_MAX) return false;
    *v = (int64_t)x; return true;
}
// A precondition the call site cannot evaluate (a call, a cast, a callee local) is REFUSED at the
// call (E137), never skipped: skipping left the callee's entry assume unlicensed.
static void ir_assert_unresolvable(LowerCtx *c) {
    ir_assert_coded(c->f, c->cur, ir_const_int(c->f, c->cur, 0, ir_type_bool(c->a)), 137);
}

// B2 (contracts): a callee's return refinement `result OP rhs` (`func f(..) usize <= m`)
// becomes a post-call `assume(v OP <that>)` — the caller LEARNS the ensures. rhs is resolved
// against the call's arguments by the same function that resolves a precondition's: a literal,
// a parameter, a parameter's `.len` (`usize < a.len`, I.1), or a sum or difference of those.
static IrValue *ir_resolve_contract_rhs(LowerCtx *c, Decl *callee, IrInstr *call, Expr *rhs, IrType *ty);  // fwd
static void ir_lower_return_ensures(LowerCtx *c, Decl *callee, IrValue *v, IrInstr *call) {
    if (!callee || callee->kind==DECL_STRUCT || !v || !v->type || v->type->kind!=IRT_INT) return;
    for (ExprList *rc = callee->as.function_decl.return_constraints; rc; rc = rc->next) {
        Expr *con = rc->expr;
        if (!con || con->kind!=EXPR_BINARY) continue;
        Expr *rhs = con->as.binary_expr.right;
        IrValue *rv = ir_resolve_contract_rhs(c, callee, call, rhs, v->type);
        if (rv && (!rv->type || rv->type->kind != IRT_INT)) rv = NULL;
        IrCmp cmp;
        if (rv && ir_tok_cmp(con->as.binary_expr.op, v->type->is_signed, &cmp)) {
            // A Lain callee PROVES its return refinement (E012 at each return); an extern's is
            // believed, so the fact is the programmer's word, not the compiler's.
            IrValue *cv = ir_icmp(c->f, c->cur, cmp, v, rv);
            if (callee->as.function_decl.body) ir_assume(c->f, c->cur, cv);
            else                               ir_assume_trusted(c->f, c->cur, cv);
        }
    }
}

// The callee side of a return refinement's right-hand side: EXACTLY the shapes
// ir_resolve_contract_rhs resolves at a call — a literal, a parameter, a parameter's `.len`, and
// `+`/`-` of those — resolved against this function's own parameters. The two must agree term
// for term: a shape the call site resolves and this one does not would license an assume nobody
// asserted. (The first cut lowered `a.len - 1` as an untyped expression here, its values came out
// `void`, the assert was silently dropped, and the caller still assumed it.)
static IrValue *ir_contract_rhs_in_callee(LowerCtx *c, Expr *rhs, IrType *ty) {
    if (!rhs) return NULL;
    { int64_t kv; if (ir_contract_const(rhs, &kv)) return ir_const_int(c->f, c->cur, kv, ty); }   // as the call site
    if (rhs->kind==EXPR_LITERAL) return ir_const_int(c->f, c->cur, rhs->as.literal_expr.value, ty);
    if (rhs->kind==EXPR_BINARY &&
        (rhs->as.binary_expr.op==TOKEN_PLUS || rhs->as.binary_expr.op==TOKEN_MINUS)) {
        IrValue *l = ir_contract_rhs_in_callee(c, rhs->as.binary_expr.left,  ty);
        IrValue *r = ir_contract_rhs_in_callee(c, rhs->as.binary_expr.right, ty);
        if (!l || !r || !l->type || l->type->kind!=IRT_INT) return NULL;
        return ir_binop(c->f, c->cur, rhs->as.binary_expr.op==TOKEN_PLUS ? IR_ADD : IR_SUB, l, r, l->type);
    }
    Id *nm = NULL; bool is_len = false;
    if (rhs->kind==EXPR_IDENTIFIER) nm = rhs->as.identifier_expr.id;
    else if (rhs->kind==EXPR_MEMBER && rhs->as.member_expr.member && rhs->as.member_expr.member->length==3
             && strncmp(rhs->as.member_expr.member->name,"len",3)==0
             && rhs->as.member_expr.target && rhs->as.member_expr.target->kind==EXPR_IDENTIFIER) {
        nm = rhs->as.member_expr.target->as.identifier_expr.id; is_len = true;
    }
    if (!nm) return NULL;
    IrLocal *l = ir_env_find(c, nm);
    IrValue *pv = (l && l->param) ? l->param : NULL;         // a parameter, as at the call site
    if (!pv) return NULL;
    if (is_len) {
        Type *at = rhs->as.member_expr.target->type;
        while (at && at->kind == TYPE_COMPTIME) at = at->element_type;
        if (at && at->kind == TYPE_ARRAY && at->array_len >= 0)   // a fixed array: its constant
            return ir_const_int(c->f, c->cur, at->array_len, ir_type_int(c->a,64,false));
        if (pv->type && pv->type->kind==IRT_ARRAY && pv->type->array_len >= 0)
            return ir_const_int(c->f, c->cur, pv->type->array_len, ir_type_int(c->a,64,false));
        return (pv->type && pv->type->kind==IRT_SLICE) ? ir_slice_len(c->f, c->cur, pv) : NULL;
    }
    return pv;
}

// B2 (ensures, CALLEE side — the soundness dual of ir_lower_return_ensures): at each
// `return e` in a function declaring `result OP rhs`, emit `assert(e OP rhs)`. The callee
// must PROVE its own postcondition; only that discharge licenses the caller's post-call
// `assume`. Without this, the caller would trust a front-end ensures the IR never checks —
// a lying/sketchy front-end could then prove a false bound from the returned value.
static void ir_lower_return_ensures_assert(LowerCtx *c, IrValue *v) {
    Decl *fn = c->fdecl;
    if (!fn || fn->kind==DECL_STRUCT || !v || !v->type || v->type->kind!=IRT_INT) return;
    for (ExprList *rc = fn->as.function_decl.return_constraints; rc; rc = rc->next) {
        Expr *con = rc->expr;
        if (!con || con->kind!=EXPR_BINARY) continue;
        Expr *rhs = con->as.binary_expr.right;
        IrValue *rv = ir_contract_rhs_in_callee(c, rhs, v->type);
        IrCmp cmp;
        if (!ir_tok_cmp(con->as.binary_expr.op, v->type->is_signed, &cmp)) continue;
        if (rv && rv->type && rv->type->kind==IRT_INT)
            ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, v, rv), 86);   // return refinement
        else
            // A right-hand side this cannot resolve (a name that is not a parameter, `.len` of a
            // non-slice): refuse it at every return rather than let it stand unchecked. The call
            // site resolves nothing for it either, so no caller assumes it.
            ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_NE, v, v), 86);
    }
}

// Resolve a contract RHS at a call site, substituting a callee param name for the
// matching call argument: a literal (typed `ty`), `m` → the arg for m, `a.len` →
// slice_len(arg for a).
static IrValue *ir_resolve_contract_rhs(LowerCtx *c, Decl *callee, IrInstr *call, Expr *rhs, IrType *ty) {
    if (!rhs) return NULL;
    { int64_t kv; if (ir_contract_const(rhs, &kv)) return ir_const_int(c->f, c->cur, kv, ty); }   // as the callee
    // ── C6, THE CALL-SITE HALF — and it is the half that keeps this sound ────────────────
    // An additive bound (`n <= cap - 1`) is rebuilt here out of the CALLER's argument values,
    // so the assert the caller must discharge is the same statement as the assume the callee
    // gets. Returning NULL for a shape this cannot resolve emits NO assert, which would leave
    // the callee believing a fact nobody proved — an unlicensed assume, the exact hazard the
    // contract machinery exists to avoid. So the two sides grow together or not at all.
    if (rhs->kind==EXPR_BINARY &&
        (rhs->as.binary_expr.op==TOKEN_PLUS || rhs->as.binary_expr.op==TOKEN_MINUS)) {
        IrValue *l = ir_resolve_contract_rhs(c, callee, call, rhs->as.binary_expr.left,  ty);
        IrValue *r = ir_resolve_contract_rhs(c, callee, call, rhs->as.binary_expr.right, ty);
        if (!l || !r || !l->type || l->type->kind!=IRT_INT) return NULL;
        IrOp op = (rhs->as.binary_expr.op==TOKEN_PLUS) ? IR_ADD : IR_SUB;
        return ir_binop(c->f, c->cur, op, l, r, l->type);
    }
    Id *nm=NULL; bool is_len=false;
    if (rhs->kind==EXPR_IDENTIFIER) nm=rhs->as.identifier_expr.id;
    else if (rhs->kind==EXPR_MEMBER && rhs->as.member_expr.member && rhs->as.member_expr.member->length==3
             && strncmp(rhs->as.member_expr.member->name,"len",3)==0
             && rhs->as.member_expr.target && rhs->as.member_expr.target->kind==EXPR_IDENTIFIER) {
        nm=rhs->as.member_expr.target->as.identifier_expr.id; is_len=true;
    }
    if (!nm) return NULL;
    int idx=0;
    for (DeclList *p=callee->as.function_decl.params; p; p=p->next, idx++) {
        if (!p->decl || p->decl->kind!=DECL_VARIABLE) continue;
        Id *pn=p->decl->as.variable_decl.name;
        if (pn && pn->length==nm->length && strncmp(pn->name,nm->name,(size_t)pn->length)==0) {
            if (idx >= call->n_operands) return NULL;
            IrValue *av = call->operands[idx];
            if (is_len) {
                // A fixed-array parameter's length is in its declared type (the argument was
                // checked against it), the same constant the callee resolves.
                Type *pt = p->decl->as.variable_decl.type;
                while (pt && pt->kind == TYPE_COMPTIME) pt = pt->element_type;
                if (pt && pt->kind == TYPE_ARRAY && pt->array_len >= 0)
                    return ir_const_int(c->f, c->cur, pt->array_len, ir_type_int(c->a,64,false));
                return (av && av->type && av->type->kind==IRT_SLICE) ? ir_slice_len(c->f,c->cur,av) : NULL;
            }
            return av;
        }
    }
    return NULL;
}
// B2 (requires): a callee's param refinements become call-site `assert`s the CALLER
// must discharge — closing the contract soundly (the callee's entry `assume` is then
// justified). Reported as VRA_PRECOND (a distinct obligation class).
static void ir_lower_call_requires(LowerCtx *c, Decl *callee, IrInstr *call) {
    if (!callee || callee->kind==DECL_STRUCT) return;
    int idx=0;
    for (DeclList *p=callee->as.function_decl.params; p; p=p->next, idx++) {
        if (!p->decl || p->decl->kind!=DECL_VARIABLE) continue;
        if (idx >= call->n_operands) break;
        IrValue *arg = call->operands[idx];
        if (!arg || !arg->type || arg->type->kind!=IRT_INT) continue;
        for (ExprList *cn=p->decl->as.variable_decl.constraints; cn; cn=cn->next) {
            Expr *con=cn->expr;
            if (!con || con->kind!=EXPR_BINARY) continue;
            IrCmp cmp;
            // `==` and `!=` are preconditions too. Skipping them left the callee's entry
            // `assume(d != 0)` (ir_lower_param_refinements) with nothing asserting it: `f(0)` to
            // `f(d i32 != 0)` was refused only by the front end's legacy E012 check, and the
            // callee's `a / d` was PROVEN on an assumption nobody discharged.
            if (con->as.binary_expr.op == TOKEN_EQUAL_EQUAL) cmp = IR_CMP_EQ;
            else if (con->as.binary_expr.op == TOKEN_BANG_EQUAL) cmp = IR_CMP_NE;
            else if (!ir_tok_cmp(con->as.binary_expr.op, arg->type->is_signed, &cmp)) continue;
            IrValue *rv = ir_resolve_contract_rhs(c, callee, call, con->as.binary_expr.right, arg->type);
            if (rv && rv->type && rv->type->kind==IRT_INT)
                ir_assert(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, arg, rv));
            else ir_assert_unresolvable(c);                      // never an unlicensed entry assume
        }
    }
}

// map an AST binary token to an IR op given signedness; returns true if arithmetic/bitwise.
static bool ir_bin_op(TokenKind t, bool sgn, IrOp *op, IrWrapMode *wrap) {
    *wrap = IR_WRAP_CHECK;
    switch (t) {
        case TOKEN_PLUS: *op=IR_ADD; return true;
        case TOKEN_MINUS:*op=IR_SUB; return true;
        case TOKEN_ASTERISK: *op=IR_MUL; return true;
        case TOKEN_SLASH:  *op= sgn?IR_SDIV:IR_UDIV; return true;
        case TOKEN_PERCENT:*op= sgn?IR_SREM:IR_UREM; return true;
        case TOKEN_AMPERSAND:*op=IR_AND; return true;
        case TOKEN_PIPE:     *op=IR_OR;  return true;
        case TOKEN_CARET:    *op=IR_XOR; return true;
        case TOKEN_SHIFT_LEFT: *op=IR_SHL; return true;
        case TOKEN_SHIFT_LEFT_PERCENT: *op=IR_SHL; *wrap=IR_WRAP_MODULAR; return true;   // bits out are discarded
        case TOKEN_SHIFT_RIGHT:*op= sgn?IR_ASHR:IR_LSHR; return true;
        case TOKEN_PLUS_PERCENT: *op=IR_ADD; *wrap=IR_WRAP_MODULAR; return true;
        case TOKEN_MINUS_PERCENT:*op=IR_SUB; *wrap=IR_WRAP_MODULAR; return true;
        case TOKEN_ASTERISK_PERCENT: *op=IR_MUL; *wrap=IR_WRAP_MODULAR; return true;
        // DECIDE-M: division with an explicit policy for the one case that overflows,
        // TYPE_MIN / -1 — `/%` wraps it to MIN, `/|` saturates it to MAX. Unsigned, both are `/`.
        case TOKEN_SLASH_PERCENT: *op= sgn?IR_SDIV:IR_UDIV; *wrap= sgn?IR_WRAP_MODULAR:IR_WRAP_CHECK; return true;
        case TOKEN_SLASH_PIPE:    *op= sgn?IR_SDIV:IR_UDIV; *wrap= sgn?IR_WRAP_SATURATE:IR_WRAP_CHECK; return true;
        default: return false;
    }
}
static bool ir_cmp_op(TokenKind t, bool sgn, IrCmp *c) {
    switch (t) {
        case TOKEN_EQUAL_EQUAL: *c=IR_CMP_EQ; return true;
        case TOKEN_BANG_EQUAL:  *c=IR_CMP_NE; return true;
        case TOKEN_ANGLE_BRACKET_LEFT:       *c= sgn?IR_CMP_SLT:IR_CMP_ULT; return true;
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: *c= sgn?IR_CMP_SLE:IR_CMP_ULE; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT:      *c= sgn?IR_CMP_SGT:IR_CMP_UGT; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:*c= sgn?IR_CMP_SGE:IR_CMP_UGE; return true;
        default: return false;
    }
}

// ── A COMPARISON MEANS WHAT IT SAYS, WHATEVER ITS OPERANDS' TYPES (I.86) ─────────────────────
// `x < n` with `x i32` and `n usize` is a question about two integers, and the IR, the
// interpreter and the octagon all answer it so (a signed compare reads each operand by its own
// type). The C it was emitted as, `v0 < v1`, is not: C converts the i32 to size_t first, so -1 < 5
// is FALSE there. A proof made on one meaning ran on the other: after `if x < n { return }`, the
// analysis knew x >= n and the C let x = -1 through, and `a[x - 4]` read out of bounds (ASan). And
// the signedness was taken from the left operand only, so `n > x` was an unsigned compare.
//
// So a mixed comparison is a SIGNED (mathematical) compare in the IR, and is lowered so that C
// computes the same thing:
//   · the signed side a non-negative literal: an unsigned compare, which C converts exactly;
//   · C promotes both to a signed type that holds them (the unsigned side narrower than 32 bits,
//     or the signed side wider than it): the plain compare, now marked signed;
//   · the unsigned side 32 bits and the signed side no wider: both widened to i64;
//   · the unsigned side 64 bits: no type holds both, so the sign is tested first. A negative
//     value is below every unsigned one; otherwise it converts exactly and the compare is
//     unsigned. Two branches, so a guard narrows each operand on each edge.
static bool ir_nonneg_lit(Expr *e) {
    return e && e->kind == EXPR_LITERAL && e->as.literal_expr.value >= 0;
}
static int ir_cbits(IrType *t) { return t->bits <= 8 ? 8 : t->bits <= 16 ? 16 : t->bits <= 32 ? 32 : 64; }
// The instruction defining v, when v was just computed in the block being lowered.
static IrInstr *ir_cmp_def(LowerCtx *c, IrValue *v) {
    for (IrInstr *i = c->cur ? c->cur->instrs : NULL; i; i = i->next) if (i->result == v) return i;
    return NULL;
}
// ★ v >= 0 BY CONSTRUCTION (I.88b): an unsigned value, a non-negative constant, a zero-extension, or a
// checked sum or product of such. That is what a guard like `(i + 15) < a.len` on a u32 i lowers
// to: the sum is computed widened, so it is SIGNED in the IR, but it cannot be negative.
static bool ir_cmp_nonneg(LowerCtx *c, IrValue *v, int depth) {
    if (!v || !v->type || v->type->kind != IRT_INT) return false;
    if (!v->type->is_signed) return true;
    IrInstr *d = depth < 4 ? ir_cmp_def(c, v) : NULL;
    if (!d) return false;
    if (d->op == IR_CONST) return d->aux.imm >= 0;
    if (d->op == IR_CAST && d->n_operands >= 1)
        return d->aux.cast_kind == IR_CAST_ZEXT ||
               (d->aux.cast_kind == IR_CAST_SEXT && ir_cmp_nonneg(c, d->operands[0], depth + 1));
    if ((d->op == IR_ADD || d->op == IR_MUL) && d->n_operands >= 2 && d->wrap == IR_WRAP_CHECK)
        return ir_cmp_nonneg(c, d->operands[0], depth + 1) && ir_cmp_nonneg(c, d->operands[1], depth + 1);
    return false;
}
// 0: a compare C gets right as written; 1: mixed, widen both to i64; 2: mixed, test the sign first;
// 3: mixed, but the signed side cannot be negative: compare unsigned, as written;
// 4: mixed, but the unsigned side is a constant that fits i64: compare signed against it.
static int ir_cmp_mixed(LowerCtx *c, IrValue *x, IrValue *y, bool xlit, bool ylit) {
    IrType *a = x ? x->type : NULL, *b = y ? y->type : NULL;
    if (!a || !b || a->kind != IRT_INT || b->kind != IRT_INT || a->is_signed == b->is_signed) return 0;
    if (a->is_signed ? xlit : ylit) return 0;
    IrType *u = a->is_signed ? b : a, *s = a->is_signed ? a : b;
    IrValue *sv = a->is_signed ? x : y, *uv = a->is_signed ? y : x;
    // ★ Before the general cases: a sign test or a widening cast in a loop guard hides the counter
    // from the termination rule (the sign test's block keeps both branches inside the loop), and
    // these two shapes need neither. `(i + 15) < a.len` and `i < a.len` over a fixed array were
    // E011 after I.88 and proved before it.
    if (ir_cmp_nonneg(c, sv, 0)) return 3;
    IrInstr *ud = ir_cmp_def(c, uv);
    if (ud && ud->op == IR_CONST && ud->aux.imm >= 0) return 4;
    (void)uv;
    if (ir_cbits(u) == 64) return 2;
    if (ir_cbits(u) == 32 && ir_cbits(s) <= 32) return 1;
    return 0;
}
static IrValue *ir_cmp_widen64(LowerCtx *c, IrValue *v) {
    if (v->type->bits == 64 && v->type->is_signed) return v;
    IrInstr *cv = ir_instr(c->f, IR_CAST, ir_type_int(c->a, 64, true), 1);
    cv->operands[0] = v;
    cv->aux.cast_kind = v->type->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT;
    ir_emit(c->cur, cv);
    return cv->result;
}
// Branch on `x OP y` where one operand is signed and the other a 64-bit unsigned value.
static void ir_cmp_br_mixed(LowerCtx *c, TokenKind op, IrValue *x, IrValue *y, IrBlock *tb, IrBlock *fb) {
    bool xs = x->type->is_signed;
    IrValue *s = xs ? x : y;
    // The answer when s < 0, i.e. when the signed operand is below every unsigned value.
    bool below = op == TOKEN_ANGLE_BRACKET_LEFT || op == TOKEN_ANGLE_BRACKET_LEFT_EQUAL;
    bool above = op == TOKEN_ANGLE_BRACKET_RIGHT || op == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL;
    bool when_neg = op == TOKEN_BANG_EQUAL || (xs ? below : above);
    IrBlock *nonneg = ir_new_block(c->f);
    ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, IR_CMP_SLT, s, ir_const_int(c->f, c->cur, 0, s->type)),
                   when_neg ? tb : fb, nonneg);
    c->cur = nonneg;
    IrType *ut = xs ? y->type : x->type;
    IrInstr *cv = ir_instr(c->f, IR_CAST, ut, 1);       // exact: s >= 0 on this edge
    cv->operands[0] = s;
    cv->aux.cast_kind = s->type->bits < ut->bits ? IR_CAST_SEXT : IR_CAST_BITCAST;
    cv->wrap = IR_WRAP_CHECK;
    ir_emit(c->cur, cv);
    IrCmp cmp = IR_CMP_EQ; ir_cmp_op(op, false, &cmp);
    ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, cmp, xs ? cv->result : x, xs ? y : cv->result), tb, fb);
}
// The comparison as a VALUE. `sgn` is used only when the operands are not two integers.
static IrValue *ir_cmp_value(LowerCtx *c, TokenKind op, IrValue *x, IrValue *y, bool xlit, bool ylit, bool sgn) {
    IrCmp cmp = IR_CMP_EQ;
    int mix = ir_cmp_mixed(c, x, y, xlit, ylit);
    if (mix == 3) {                 // x >= 0 converts exactly to the unsigned side
        ir_cmp_op(op, false, &cmp);
        return ir_icmp(c->f, c->cur, cmp, x, y);
    }
    if (mix == 4) {                 // the unsigned side is a constant k <= INT64_MAX: compare signed
        bool xs = x->type->is_signed;
        IrInstr *ud = ir_cmp_def(c, xs ? y : x);
        IrValue *k = ir_const_int(c->f, c->cur, ud->aux.imm, ir_type_int(c->a, 64, true));
        ir_cmp_op(op, true, &cmp);
        return ir_icmp(c->f, c->cur, cmp, xs ? x : k, xs ? k : y);
    }
    if (mix == 1) {
        IrValue *wx = ir_cmp_widen64(c, x), *wy = ir_cmp_widen64(c, y);
        ir_cmp_op(op, true, &cmp);
        return ir_icmp(c->f, c->cur, cmp, wx, wy);
    }
    if (mix == 2) {
        IrType *bt = ir_type_bool(c->a);
        IrValue *cell = ir_alloca(c->f, c->cur, bt);
        IrBlock *yes = ir_new_block(c->f), *no = ir_new_block(c->f), *jn = ir_new_block(c->f);
        ir_cmp_br_mixed(c, op, x, y, yes, no);
        c->cur = yes; ir_store(c->f, c->cur, cell, ir_const_int(c->f, c->cur, 1, bt)); ir_set_br(c->cur, jn);
        c->cur = no;  ir_store(c->f, c->cur, cell, ir_const_int(c->f, c->cur, 0, bt)); ir_set_br(c->cur, jn);
        c->cur = jn;
        return ir_load(c->f, c->cur, cell, bt);
    }
    IrType *a = x ? x->type : NULL, *b = y ? y->type : NULL;
    if (a && b && a->kind == IRT_INT && b->kind == IRT_INT) {
        if (a->is_signed == b->is_signed) sgn = a->is_signed;
        else sgn = !(a->is_signed ? xlit : ylit);   // unsigned only against a non-negative literal
    }
    ir_cmp_op(op, sgn, &cmp);
    return ir_icmp(c->f, c->cur, cmp, x, y);
}
// The comparison as a BRANCH: the mixed 64-bit case branches directly, so each edge keeps its fact.
static void ir_cmp_br(LowerCtx *c, TokenKind op, IrValue *x, IrValue *y, bool xlit, bool ylit, bool sgn,
                      IrBlock *tb, IrBlock *fb) {
    if (ir_cmp_mixed(c, x, y, xlit, ylit) == 2) { ir_cmp_br_mixed(c, op, x, y, tb, fb); return; }
    ir_set_br_cond(c->cur, ir_cmp_value(c, op, x, y, xlit, ylit, sgn), tb, fb);
}

// Recursively resolve a callee-scope length expression at a CALL SITE into caller values —
// each callee param name is substituted by the matching call argument (ident / `x.len` via
// ir_resolve_contract_rhs; +/-/* recurse), and a constant is its value. NULL if any leaf is
// unresolvable, and the caller then REFUSES the call (ir_assert_unresolvable): the callee's entry
// assumes the length whatever happens here, so emitting nothing was not "fail-closed" but an
// unlicensed assume.
static IrValue *ir_resolve_len_expr(LowerCtx *c, Decl *callee, IrInstr *call, Expr *e, IrType *ty) {
    if (!e) return NULL;
    { int64_t kv; if (ir_contract_const(e, &kv)) return ir_const_int(c->f, c->cur, kv, ty); }
    if (e->kind==EXPR_BINARY) {
        IrOp op; IrWrapMode wrap;
        if (!ir_bin_op(e->as.binary_expr.op, false, &op, &wrap)) return NULL;
        IrValue *l = ir_resolve_len_expr(c, callee, call, e->as.binary_expr.left, ty);
        IrValue *r = ir_resolve_len_expr(c, callee, call, e->as.binary_expr.right, ty);
        if (!l || !r) return NULL;
        return ir_binop(c->f, c->cur, op, l, r, ty);
    }
    return ir_resolve_contract_rhs(c, callee, call, e, ty);   // literal / param ident / x.len
}

// C (dependent-length requires — the soundness dual of ir_lower_slice_len_refinement): at a
// call, for each callee param declared as a sized slice `T[expr]`, assert
// `len(arg) relop resolve(expr)` (equality ⇒ both directions, matching the entry assume).
// This licenses the callee's entry `assume(len == expr)`; without it the callee would trust
// that the caller passed a correctly-sized buffer — a front-end that let a short slice
// through would make the callee prove a false `out[i]` bound. Fail-closed if unresolvable.
static void ir_lower_call_slice_len_requires(LowerCtx *c, Decl *callee, IrInstr *call) {
    if (!callee || callee->kind==DECL_STRUCT) return;
    int idx=0;
    for (DeclList *p=callee->as.function_decl.params; p; p=p->next, idx++) {
        if (!p->decl || p->decl->kind!=DECL_VARIABLE) continue;
        if (idx >= call->n_operands) break;
        Type *pty = p->decl->as.variable_decl.type;
        if (!pty || pty->kind!=TYPE_ARRAY || pty->array_len>=0 || !pty->size_expr) continue;
        IrValue *arg = call->operands[idx];
        if (!arg || !arg->type || arg->type->kind!=IRT_SLICE) continue;
        IrValue *L  = ir_slice_len(c->f, c->cur, arg);
        IrValue *rv = ir_resolve_len_expr(c, callee, call, pty->size_expr, ir_type_int(c->a,64,false));
        // Fail-CLOSED means refused. This said `continue // fail-closed` and emitted nothing, while
        // the callee's entry still assumed the length: an unlicensed assume (see ir_contract_const).
        if (!rv || !rv->type || rv->type->kind!=IRT_INT) { ir_assert_unresolvable(c); continue; }
        if (pty->size_relop == TOKEN_EQUAL_EQUAL) {                 // len == expr ⇒ both dirs
            ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_UGE, L, rv), 87);
            ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_ULE, L, rv), 87);
        } else { IrCmp cmp;
            if (ir_tok_cmp(pty->size_relop, false, &cmp))
                ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, L, rv), 87);
        }
    }
}

// ── A CONSTANT-LENGTH ARRAY PARAMETER IS A LENGTH PRECONDITION TOO ─────────────────────────
// `func last(a i32[8]) i32 { return a[7] }` proves a[7] from the TYPE: the callee's body is
// right to trust that `a` has 8 elements. Nothing at the call site made that true. The sized
// form `T[expr]` asserts `len(arg) relop expr` above, but a constant N (`array_len >= 0`, no
// size_expr) was skipped, and only an array LITERAL argument was measured (front end, E012):
//     var one i32[1] = [5]; return last(one)        compiled, and read 7 elements past the end
//     setlast(var one) with `var a i32[8]`           compiled, and WROTE past the end
//     last(b.d) on a field `d i32[1]`                compiled, same
//     last(s) on a slice `s i32[]`                   emitted C gcc rejects (2 args for 1)
// (ASan: stack-buffer-overflow; found 2026-09-29 by fuzz_bsearch's slice carrier.)
//
// The obligation is `len(arg) >= N`, not `==`: the callee touches N elements and no more, so a
// longer array is safe, and passing a prefix of a bigger array stays legal as it always was.
// A fixed-array argument's length is a constant from its source TYPE; a slice's is its runtime
// length, and the slice's DATA pointer is what a fixed-array parameter receives. An argument
// whose length cannot be read is refused (assert false): trusting it is the hole being closed.
static void ir_lower_call_fixed_len_requires(LowerCtx *c, Decl *callee, IrInstr *call, ExprList *args) {
    if (!callee || callee->kind==DECL_STRUCT) return;
    int idx = 0;
    ExprList *a = args;
    for (DeclList *p=callee->as.function_decl.params; p && a; p=p->next, a=a->next, idx++) {
        if (idx >= call->n_operands) break;
        if (!p->decl || p->decl->kind!=DECL_VARIABLE) continue;
        Type *pty = p->decl->as.variable_decl.type;
        if (!pty || pty->kind!=TYPE_ARRAY || pty->array_len<0 || pty->size_expr) continue;
        IrValue *arg = call->operands[idx];
        if (!arg || !arg->type) continue;
        Expr *ax = a->expr;
        while (ax && (ax->kind==EXPR_MUT || ax->kind==EXPR_MOVE))          // `var x` / `mov x` pass x
            ax = ax->kind==EXPR_MUT ? ax->as.mut_expr.expr : ax->as.move_expr.expr;
        IrType *u64 = ir_type_int(c->a, 64, false);
        if (arg->type->kind==IRT_SLICE) {                                 // runtime length: prove it
            IrValue *L = ir_slice_len(c->f, c->cur, arg);
            IrValue *N = ir_const_int(c->f, c->cur, pty->array_len, u64);
            ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_UGE, L, N), 87);
            IrType *el = ir_lower_type(c, pty->element_type);
            call->operands[idx] = ir_slice_data(c->f, c->cur, arg, el ? el : arg->type->elem);
            continue;
        }
        int64_t len = -1;                                                 // a constant: decide it HERE
        if (ax && ax->type && ax->type->kind==TYPE_ARRAY && ax->type->array_len>=0) len = ax->type->array_len;
        else if (arg->type->kind==IRT_ARRAY && arg->type->array_len>=0)    len = arg->type->array_len;
        // ★ NOTHING IS EMITTED WHEN THE CONSTANT SUFFICES. `drain(mov a)` on an aggregate lowers
        // to `consume %a; call drain(%a)`, and linearity accepts the call's use of %a only when
        // the consume is the instruction RIGHT BEFORE it (D-35). A statically true assert
        // between the two turned every whole-array move into a false E001.
        if (len >= pty->array_len) continue;
        // Too short, or a length that cannot be read (fail CLOSED): a refuted assert. Rejected
        // either way, so what it sits between no longer matters.
        IrValue *L = ir_const_int(c->f, c->cur, len < 0 ? 0 : len, u64);
        IrValue *N = ir_const_int(c->f, c->cur, pty->array_len, u64);
        ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_UGE, L, N), 87);
    }
}

// address of an lvalue (identifier slot / index / member) — for assignment + index.
// `p.f` where `p : *T` implicitly DEREFERENCES, exactly as C's `->` does. The lowering used
// the target's own type for the field lookup, so a member of a pointer-to-struct resolved to
// nothing and became an OPAQUE read of unknown storage — which then suppressed every proof in
// the whole function. A pointer-to-struct target's ADDRESS is the pointer value itself.
static IrType *ir_struct_of(IrType *t) {
    if (t && t->kind==IRT_PTR && t->elem && (t->elem->kind==IRT_STRUCT || t->elem->kind==IRT_SUM))
        return t->elem;
    return t;
}

static IrValue *ir_lower_addr(LowerCtx *c, Expr *e) {
    if (e->kind == EXPR_IDENTIFIER) {
        IrLocal *l = ir_env_find(c, e->as.identifier_expr.id);
        IrType  *rp = ir_ref_binding_ptr(l);
        if (rp) return ir_load(c->f, c->cur, l->slot, rp);    // the REFERENT's address
        if (l && l->slot) return l->slot;
        // A binding that is a VALUE with no home slot — a match-arm payload (`case s { Pt(p):
        // ... p.x ... }`) or a by-value parameter — still needs an address when a field is
        // read off it. It had none, so `p.x` lowered to a field of an OPAQUE null and the
        // emitted C dereferenced NULL: three ADT tests SEGFAULTED. Materialise a temporary,
        // which is the ordinary C rule for a temporary's lifetime and is already what the
        // rvalue path below does. Such a binding is immutable, so nothing is lost by copying.
        if (l && l->param && l->param->type) {
            IrValue *tmp = ir_alloca(c->f, c->cur, l->param->type);
            ir_store(c->f, c->cur, tmp, l->param);
            return tmp;
        }
    }
    // `*p` as an LVALUE: the address IS the pointer. Without this every `*p = v` produced an
    // OPAQUE placeholder and the write was silently LOST — `unsafe { *q = *q + 100 }` computed
    // nothing at all. The dereference is exactly the identity on addresses.
    if (e->kind == EXPR_DEREF) return ir_lower_expr(c, e->as.deref_expr.expr);
    if (e->kind == EXPR_INDEX) {
        Expr    *tgt  = e->as.index_expr.target;
        IrType  *tty  = (tgt && tgt->type) ? ir_lower_type(c, tgt->type) : NULL;
        IrValue *base;
        // ★ A LANE IS ADDRESSED IN THE VECTOR, NOT IN A COPY OF IT. An array has no value in
        // this model, so lowering it yields its address; a vector IS a value, so lowering it
        // LOADED a copy and the lane pointer pointed into that copy. A read got the right lane;
        // `v[2] = 30` wrote the copy and was silently lost (the program returned the old
        // lane, at -O0 as at -O2). An addressable vector is addressed; an rvalue one is
        // materialised, the rule EXPR_MEMBER already follows for a struct.
        if (tty && tty->kind == IRT_VECTOR) {
            bool addressable = tgt->kind==EXPR_IDENTIFIER || tgt->kind==EXPR_MEMBER
                            || tgt->kind==EXPR_INDEX     || tgt->kind==EXPR_DEREF;
            if (addressable) base = ir_lower_addr(c, tgt);
            else {
                IrValue *v = ir_lower_expr(c, tgt);
                base = ir_alloca(c->f, c->cur, tty);
                ir_store(c->f, c->cur, base, v);
            }
        } else base = ir_lower_expr(c, tgt);
        IrValue *idx  = ir_lower_expr(c, e->as.index_expr.index);
        IrType  *elem = ir_lower_type(c, e->type);
        if (base->type && base->type->kind == IRT_SLICE)
            base = ir_slice_data(c->f, c->cur, base, elem);   // index through .data
        IrValue *p = ir_elem_ptr(c->f, c->cur, base, idx, elem);
        p->line = e->line; p->col = e->col;
        c->cur->instrs_tail->unchecked = c->unsafe;   // elem_ptr just emitted
        return p;
    }
    if (e->kind == EXPR_MEMBER) {
        Expr   *tgt  = e->as.member_expr.target;
        IrType *sty0 = ir_lower_type(c, tgt->type);
        IrType *sty  = ir_struct_of(sty0);
        bool    thru = (sty != sty0);          // through a pointer: the value IS the address
        IrType *fty  = NULL;
        int idx = ir_field_index(sty, e->as.member_expr.member, &fty);
        if (idx >= 0) {
            // The target may be an RVALUE — `mk(i).x` takes a field of a RETURNED struct.
            // An rvalue has no address, so MATERIALIZE it into a temporary and address that,
            // which is the ordinary C rule for a temporary's lifetime. Falling through to the
            // placeholder instead emitted an UNINITIALISED slot and read the field out of
            // garbage: the call never happened and the program silently computed nonsense.
            bool addressable = tgt->kind==EXPR_IDENTIFIER || tgt->kind==EXPR_MEMBER
                            || tgt->kind==EXPR_INDEX     || tgt->kind==EXPR_DEREF;
            IrValue *base;
            if (thru) base = ir_lower_expr(c, tgt);
            else if (addressable) base = ir_lower_addr(c, tgt);
            else {
                IrValue *v = ir_lower_expr(c, tgt);
                base = ir_alloca(c->f, c->cur, sty);
                ir_store(c->f, c->cur, base, v);
            }
            return ir_field_ptr(c->f, c->cur, base, idx, fty);
        }
    }
    // An lvalue whose ADDRESS we cannot compute. A fresh alloca was actively WRONG: writes
    // through it went to a scratch local and were silently lost, reads returned garbage. An
    // opaque POINTER is the honest model — the address is unknown, so anything written
    // through it may touch anything, and `ir_place_of` resolves it to an unattributable
    // DEREF which every conflict rule already treats as "may alias".
    { IrType *pt = ir_type_new(c->a, IRT_PTR);
      pt->elem = ir_lower_type(c, e->type); pt->ptr_mut = true; pt->is_raw = true;   // `&x`
      return ir_opaque(c->f, c->cur, pt, true, "unlowered-lvalue", NULL, 0); }
}

static IrValue *ir_lower_expr_raw(LowerCtx *c, Expr *e);
static void ir_lower_range_member(LowerCtx *c, Expr *e, IrBlock *tb, IrBlock *fb);
static void ir_range_member_br(LowerCtx *c, Expr *R, IrValue *x, IrBlock *tb, IrBlock *fb);
static void ir_list_member_br(LowerCtx *c, Expr *R, IrValue *x, IrBlock *tb, IrBlock *fb);

// D-49's seam. While a loop CONDITION is lowered, every pure subexpression's value is
// remembered; while its MEASURE is lowered, a structurally identical one is reused rather than
// recomputed, so `while i < n / 2 decreasing n / 2 - i` mentions ONE `n / 2` in the IR.
//
// Scoped to that one statement on purpose. A general CSE over the function would be a bigger
// change with a bigger blast radius, and the problem is not general: it is that a guard and its
// own measure are two spellings of the same quantities, written in one line by one programmer.

// ── SATURATING ARITHMETIC AT 64 BITS ────────────────────────────────────────────────────────
// The ≤32-bit expansion widens to i64 and clamps; at 64 bits there is no wider type, so `x +| 1`
// on a usize was E100 "unhandled-binop" — the index type, where clamping is most wanted. Here the
// OVERFLOW TEST comes first and the operation second, each operation guarded so it cannot
// overflow (the CERT INT32-C forms), and so each carries no obligation of its own (`unchecked`,
// as in `unsafe`: the guard IS the proof). The result is stored to a cell from each arm.
static IrValue *ir_sat_op(LowerCtx *c, IrOp op, IrValue *a, IrValue *b, IrType *t) {
    IrValue *r = ir_binop(c->f, c->cur, op, a, b, t);
    if (c->cur->instrs_tail) c->cur->instrs_tail->unchecked = true;
    return r;
}
// if (cond) { cell = then_v } else { continue in a fresh block }: returns the else block.
static void ir_sat_arm(LowerCtx *c, IrValue *cond, IrValue *cell, IrValue *then_v, IrBlock *join) {
    IrBlock *tb = ir_new_block(c->f), *eb = ir_new_block(c->f);
    ir_set_br_cond(c->cur, cond, tb, eb);
    c->cur = tb; ir_store(c->f, c->cur, cell, then_v); ir_set_br(c->cur, join);
    c->cur = eb;
}
static IrValue *ir_sat_cast(LowerCtx *c, IrValue *v, IrType *t) {
    if (!v || !v->type || v->type == t || (v->type->kind==IRT_INT && v->type->bits==t->bits &&
                                           v->type->is_signed==t->is_signed)) return v;
    IrInstr *cv = ir_instr(c->f, IR_CAST, t, 1); cv->operands[0] = v;
    cv->aux.cast_kind = (v->type->kind==IRT_INT && v->type->is_signed) ? IR_CAST_SEXT : IR_CAST_ZEXT;
    if (v->type->kind==IRT_INT && v->type->bits > t->bits) cv->aux.cast_kind = IR_CAST_TRUNC;
    ir_emit(c->cur, cv);
    return cv->result;
}
// An OVERFLOW arm: saturating, store the bound and join; checked (`bad` set), leave for the
// `else` arm. A shortcut arm (a factor is 0) is not an overflow and always stores its value.
static void ir_ovf_arm(LowerCtx *c, IrValue *cond, IrValue *cell, IrValue *then_v, IrBlock *join,
                       IrBlock *bad) {
    if (!bad) { ir_sat_arm(c, cond, cell, then_v, join); return; }
    IrBlock *eb = ir_new_block(c->f);
    ir_set_br_cond(c->cur, cond, bad, eb);
    c->cur = eb;
}
// ── THE GUARDED EXPANSION, AT ANY WIDTH ─────────────────────────────────────────────────────
// Written for 64 bits, where there is no wider type to compute in, and now the expansion for
// every width the widen-and-clamp form cannot serve: 33..63 (E100 "unhandled-binop" until
// 2026-09-28 — `i48 +| 1`, `u33 +? x else 0`) and u32 MULTIPLY, whose product needs 64 bits of
// magnitude and so does not fit the signed i64 it was computed in (fuzz_wrap: 4294967295 *
// 4294967294, a C signed overflow under UBSan). `op` is the saturating token; `bad`, when set,
// makes it the CHECKED operator: overflow leaves for the caller's `else` arm instead of clamping.
static IrValue *ir_lower_guarded(LowerCtx *c, TokenKind op, IrValue *a0, IrValue *b0, IrType *t,
                                 int64_t lo, int64_t hi, IrBlock *bad) {
    IrValue *a = ir_sat_cast(c, a0, t), *b = ir_sat_cast(c, b0, t);
    IrValue *cell = ir_alloca(c->f, c->cur, t);
    IrBlock *join = ir_new_block(c->f);
    IrValue *MX = ir_const_int(c->f, c->cur, hi, t), *MN = ir_const_int(c->f, c->cur, lo, t);
    IrValue *Z  = ir_const_int(c->f, c->cur, 0, t);
    bool sg = t->is_signed;
    // u64's MAX is 2^64-1, which the numeric domain (i64-based: irtype_int_range clamps an
    // unsigned 64-bit type to INT64_MAX) cannot represent. Taken from `hi` it saturated at 2^63-1;
    // written as a constant it would read as -1 and could make an arm look dead. Computed as
    // `0 -% 1` it is the right bit pattern, and to the analysis an honest unknown. Below 64 bits
    // `hi` is exact and stays a constant.
    if (!sg && t->bits >= 64) {
        IrValue *one = ir_const_int(c->f, c->cur, 1, t);
        MX = ir_binop(c->f, c->cur, IR_SUB, Z, one, t);
        if (c->cur->instrs_tail) c->cur->instrs_tail->wrap = IR_WRAP_MODULAR;
    }
    IrCmp GT = sg ? IR_CMP_SGT : IR_CMP_UGT, LT = sg ? IR_CMP_SLT : IR_CMP_ULT;
    if (!sg) {
        if (op == TOKEN_PLUS_PIPE) {            // a > MAX - b  →  MAX
            ir_ovf_arm(c, ir_icmp(c->f, c->cur, GT, a, ir_sat_op(c, IR_SUB, MX, b, t)), cell, MX, join, bad);
            ir_store(c->f, c->cur, cell, ir_sat_op(c, IR_ADD, a, b, t));
        } else if (op == TOKEN_MINUS_PIPE) {    // a < b  →  0
            ir_ovf_arm(c, ir_icmp(c->f, c->cur, LT, a, b), cell, Z, join, bad);
            ir_store(c->f, c->cur, cell, ir_sat_op(c, IR_SUB, a, b, t));
        } else {                                // b != 0 && a > MAX / b  →  MAX
            ir_sat_arm(c, ir_icmp(c->f, c->cur, IR_CMP_EQ, b, Z), cell, Z, join);
            ir_ovf_arm(c, ir_icmp(c->f, c->cur, GT, a, ir_sat_op(c, IR_UDIV, MX, b, t)), cell, MX, join, bad);
            ir_store(c->f, c->cur, cell, ir_sat_op(c, IR_MUL, a, b, t));
        }
    } else if (op == TOKEN_PLUS_PIPE || op == TOKEN_MINUS_PIPE) {
        bool add = (op == TOKEN_PLUS_PIPE);
        // add: b > 0 may overflow up (a > MAX - b), b <= 0 down (a < MIN - b)
        // sub: b < 0 may overflow up (a > MAX + b), b >= 0 down (a < MIN + b)
        IrBlock *pos = ir_new_block(c->f), *neg = ir_new_block(c->f);
        IrValue *up_side = add ? ir_icmp(c->f, c->cur, GT, b, Z) : ir_icmp(c->f, c->cur, LT, b, Z);
        ir_set_br_cond(c->cur, up_side, pos, neg);
        c->cur = pos;
        ir_ovf_arm(c, ir_icmp(c->f, c->cur, GT, a, ir_sat_op(c, add ? IR_SUB : IR_ADD, MX, b, t)), cell, MX, join, bad);
        ir_store(c->f, c->cur, cell, ir_sat_op(c, add ? IR_ADD : IR_SUB, a, b, t)); ir_set_br(c->cur, join);
        c->cur = neg;
        ir_ovf_arm(c, ir_icmp(c->f, c->cur, LT, a, ir_sat_op(c, add ? IR_SUB : IR_ADD, MN, b, t)), cell, MN, join, bad);
        ir_store(c->f, c->cur, cell, ir_sat_op(c, add ? IR_ADD : IR_SUB, a, b, t));
    } else {                                    // signed *|: CERT INT32-C, by the operands' signs
        IrBlock *apos = ir_new_block(c->f), *anp = ir_new_block(c->f);
        ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, GT, a, Z), apos, anp);
        IrBlock *mul = ir_new_block(c->f);
        c->cur = apos; {                        // a > 0
            IrBlock *bp = ir_new_block(c->f), *bn = ir_new_block(c->f);
            ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, GT, b, Z), bp, bn);
            c->cur = bp;  ir_ovf_arm(c, ir_icmp(c->f, c->cur, GT, a, ir_sat_op(c, IR_SDIV, MX, b, t)), cell, MX, join, bad); ir_set_br(c->cur, mul);
            c->cur = bn;  ir_ovf_arm(c, ir_icmp(c->f, c->cur, LT, b, ir_sat_op(c, IR_SDIV, MN, a, t)), cell, MN, join, bad); ir_set_br(c->cur, mul);
        }
        c->cur = anp; {                         // a <= 0
            IrBlock *bp = ir_new_block(c->f), *bn = ir_new_block(c->f);
            ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, GT, b, Z), bp, bn);
            c->cur = bp;  ir_ovf_arm(c, ir_icmp(c->f, c->cur, LT, a, ir_sat_op(c, IR_SDIV, MN, b, t)), cell, MN, join, bad); ir_set_br(c->cur, mul);
            c->cur = bn;  {                     // a <= 0, b <= 0: overflow iff a != 0 && b < MAX / a
                ir_sat_arm(c, ir_icmp(c->f, c->cur, IR_CMP_EQ, a, Z), cell, Z, join);
                ir_ovf_arm(c, ir_icmp(c->f, c->cur, LT, b, ir_sat_op(c, IR_SDIV, MX, a, t)), cell, MX, join, bad);
                ir_set_br(c->cur, mul);
            }
        }
        c->cur = mul;
        ir_store(c->f, c->cur, cell, ir_sat_op(c, IR_MUL, a, b, t));
    }
    ir_set_br(c->cur, join);
    c->cur = join;
    return ir_load(c->f, c->cur, cell, t);
}
// The widen-and-clamp form computes in i64, which holds every ±, and every × but u32 × u32.
static bool ir_widen_serves(IrType *t, TokenKind op) {
    return t->bits <= 32 && !(!t->is_signed && t->bits == 32 &&
                              (op == TOKEN_ASTERISK_PIPE || op == TOKEN_ASTERISK_QUESTION));
}
static TokenKind ir_checked_as_sat(TokenKind op) {
    return op == TOKEN_PLUS_QUESTION ? TOKEN_PLUS_PIPE
         : op == TOKEN_MINUS_QUESTION ? TOKEN_MINUS_PIPE : TOKEN_ASTERISK_PIPE;
}

static IrValue *ir_lower_expr(LowerCtx *c, Expr *e) {
    if (c && c->cse_n > 0 && e && !c->cse_recording) {
        for (int i = 0; i < c->cse_n; i++)
            if (ir_expr_same(c->cse_expr[i], e)) return c->cse_val[i];
    }
    // A node's own instructions are emitted AFTER its operands are lowered, and each operand
    // moved the current position to itself: `i = i + 1` reported its add at the `1`. Restore
    // the caller's position on the way out, so the add is at its operator.
    isize sv_line = ir_cur_line, sv_col = ir_cur_col;
    IrValue *v = ir_lower_expr_raw(c, e);
    ir_cur_line = sv_line; ir_cur_col = sv_col;
    if (c && c->cse_recording && e && v && c->cse_n < 32) {
        switch (e->kind) {                      // only the shapes ir_expr_same can match
            case EXPR_BINARY: case EXPR_MEMBER: case EXPR_IDENTIFIER:
                c->cse_expr[c->cse_n] = e; c->cse_val[c->cse_n] = v; c->cse_n++;
                break;
            default: break;
        }
    }
    return v;
}

static IrValue *ir_lower_expr_raw(LowerCtx *c, Expr *e) {
    if (e && e->line) { ir_cur_line = e->line; ir_cur_col = e->col; }
    if (!e) return ir_const_int(c->f, c->cur, 0, ir_type_int(c->a,32,true));
    IrType *ty = ir_lower_type(c, e->type);
    switch (e->kind) {
        // ── ERROR HANDLING: `try e` and `e else arm` ──────────────────────────────────────
        // Both ask the same question of a `T | m1 | m2` value — is it the payload or a marker?
        // — and differ only in what they do on the marker side. Neither had a lowering at all,
        // so both became OPAQUE placeholders: the value was never computed and the `errors/`
        // family printed garbage. With TYPE_UNION now a sum, the question is a tag test and
        // the answer is a payload projection.
        //
        // Variant 0 is the payload by construction (see ir_lower_type_impl), so "is a marker"
        // is exactly `tag != 0`.
        case EXPR_TRY: case EXPR_ELSE: {
            bool is_try = (e->kind == EXPR_TRY);
            Expr *opx = is_try ? e->as.try_expr.operand : e->as.else_expr.operand;
            // ★ A CHECKED OPERATOR is the other thing `else` handles, and it was not lowered
            // at all: `a +? b else panic(...)` produced two OPAQUEs and the variable held
            // garbage. It has no union to test — the failure is an OVERFLOW — so build the
            // test rather than look for a tag: compute in a WIDER type, ask whether the result
            // fits back in the operand type, and branch. Stating it that way means the numeric
            // domain sees a real comparison and can often prove the `else` arm DEAD, which is
            // the whole point of a checked operator in a language that proves overflow away.
            if (!is_try && opx && expr_is_checked_op(opx)) {
                IrType *rt2 = ir_lower_type(c, opx->type);
                int64_t tlo, thi;
                // ★ BEYOND THE WIDENED FORM. `+? -? *?` computed in i64, so they stopped at 32 bits
                // (E100 above) and u32 `*?` overflowed i64 itself; `as?` widened its SOURCE to i64,
                // so a u64 above i64::MAX became negative and "fit" — `x as? i32 else 0` on 2^64−1
                // gave −1. Where the widened form does not serve, the operator is the guarded
                // expansion with its overflow arms routed to `else`, and `as?` compares the source
                // against the target's bounds IN THE SOURCE'S OWN TYPE, where every value is exact.
                bool wide_ok = rt2 && rt2->kind==IRT_INT && irtype_int_range(rt2, &tlo, &thi) &&
                               (opx->kind == EXPR_CAST ? false
                                : ir_widen_serves(rt2, opx->as.binary_expr.op));
                if (!wide_ok && rt2 && rt2->kind==IRT_INT && rt2->bits <= 64 &&
                    irtype_int_range(rt2, &tlo, &thi)) {
                    IrValue *cell2 = ir_alloca(c->f, c->cur, rt2);
                    IrBlock *bad2 = ir_new_block(c->f), *jn2 = ir_new_block(c->f);
                    if (opx->kind == EXPR_CAST) {
                        IrValue *sv = ir_lower_expr(c, opx->as.cast_expr.expr);
                        IrType *S = sv ? sv->type : NULL;
                        if (!S || S->kind != IRT_INT)
                            return ir_opaque_expr(c, ty, false, "checked-cast-non-int", NULL, NULL);
                        int sb = S->bits, db = rt2->bits;
                        __int128 smin = S->is_signed ? -((__int128)1 << (sb-1)) : 0;
                        __int128 smax = S->is_signed ? ((__int128)1 << (sb-1)) - 1 : ((__int128)1 << sb) - 1;
                        __int128 dmax = rt2->is_signed ? ((__int128)1 << (db-1)) - 1 : ((__int128)1 << db) - 1;
                        __int128 lo2 = tlo, hi2 = (!rt2->is_signed && db >= 64) ? dmax : (__int128)thi;
                        if (lo2 > smin) {             // below the target: only a signed source
                            IrBlock *nb = ir_new_block(c->f);
                            ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, IR_CMP_SLT, sv,
                                           ir_const_int(c->f, c->cur, (int64_t)lo2, S)), bad2, nb);
                            c->cur = nb;
                        }
                        if (hi2 < smax) {             // above the target, compared in S's signedness
                            IrBlock *nb = ir_new_block(c->f);
                            ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, S->is_signed ? IR_CMP_SGT : IR_CMP_UGT,
                                           sv, ir_const_int(c->f, c->cur, (int64_t)hi2, S)), bad2, nb);
                            c->cur = nb;
                        }
                        IrInstr *nr = ir_instr(c->f, IR_CAST, rt2, 1); nr->operands[0] = sv;
                        nr->aux.cast_kind = sb > db ? IR_CAST_TRUNC : sb < db
                                          ? (S->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT) : IR_CAST_BITCAST;
                        nr->unchecked = true;         // the two tests above ARE the proof
                        ir_emit(c->cur, nr);
                        ir_store(c->f, c->cur, cell2, nr->result);
                    } else {
                        IrValue *la = ir_lower_expr(c, opx->as.binary_expr.left);
                        IrValue *ra = ir_lower_expr(c, opx->as.binary_expr.right);
                        IrValue *okv = ir_lower_guarded(c, ir_checked_as_sat(opx->as.binary_expr.op),
                                                        la, ra, rt2, tlo, thi, bad2);
                        ir_store(c->f, c->cur, cell2, okv);
                    }
                    ir_set_br(c->cur, jn2);
                    c->cur = bad2;
                    if (e->as.else_expr.arm_is_return) {
                        IrValue *rv = e->as.else_expr.arm ? ir_lower_expr(c, e->as.else_expr.arm) : NULL;
                        ir_lower_flush_defers(c);
                        if (!ir_is_set_term(c->cur)) ir_set_ret(c->cur, rv);   // a defer may end it
                    } else {
                        IrValue *av = e->as.else_expr.arm ? ir_lower_expr(c, e->as.else_expr.arm) : NULL;
                        if (e->as.else_expr.is_panic) ir_set_unreachable(c->cur);
                        else { if (av) ir_store(c->f, c->cur, cell2, av); ir_set_br(c->cur, jn2); }
                    }
                    c->cur = jn2;
                    return ir_load(c->f, c->cur, cell2, rt2);
                }
                if (wide_ok) {
                    IrType *w = ir_type_int(c->a, 64, true);            // ℤ-widened operands
                    IrValue *wide = NULL;
                    {
                    IrValue *la = ir_lower_expr(c, opx->as.binary_expr.left);
                    IrValue *ra = ir_lower_expr(c, opx->as.binary_expr.right);
                    // Widen by the OPERAND's signedness: sign-extending a u32 near its top
                    // would make it negative and the fits-test would answer about a different
                    // number than the program computes.
                    IrCastKind ck = rt2->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT;
                    IrInstr *cl = ir_instr(c->f, IR_CAST, w, 1); cl->operands[0]=la;
                        cl->aux.cast_kind=ck; ir_emit(c->cur, cl);
                    IrInstr *cr = ir_instr(c->f, IR_CAST, w, 1); cr->operands[0]=ra;
                        cr->aux.cast_kind=ck; ir_emit(c->cur, cr);
                    IrOp op = opx->as.binary_expr.op==TOKEN_PLUS_QUESTION  ? IR_ADD
                            : opx->as.binary_expr.op==TOKEN_MINUS_QUESTION ? IR_SUB : IR_MUL;
                    wide = ir_binop(c->f, c->cur, op, cl->result, cr->result, w);
                    }
                    IrValue *ge = ir_icmp(c->f, c->cur, IR_CMP_SGE, wide, ir_const_int(c->f,c->cur,tlo,w));
                    IrValue *le = ir_icmp(c->f, c->cur, IR_CMP_SLE, wide, ir_const_int(c->f,c->cur,thi,w));
                    IrValue *cell2 = ir_alloca(c->f, c->cur, rt2);
                    IrBlock *hi2 = ir_new_block(c->f), *okb2 = ir_new_block(c->f),
                            *bad2 = ir_new_block(c->f), *jn2 = ir_new_block(c->f);
                    ir_set_br_cond(c->cur, ge, hi2, bad2);
                    c->cur = hi2; ir_set_br_cond(c->cur, le, okb2, bad2);
                    c->cur = okb2;
                    { IrInstr *nr = ir_instr(c->f, IR_CAST, rt2, 1); nr->operands[0]=wide;
                      nr->aux.cast_kind=IR_CAST_TRUNC; ir_emit(c->cur, nr);
                      ir_store(c->f, c->cur, cell2, nr->result); }
                    ir_set_br(c->cur, jn2);
                    c->cur = bad2;
                    if (e->as.else_expr.arm_is_return) {
                        IrValue *rv = e->as.else_expr.arm ? ir_lower_expr(c, e->as.else_expr.arm) : NULL;
                        ir_lower_flush_defers(c);
                        if (!ir_is_set_term(c->cur)) ir_set_ret(c->cur, rv);   // a defer may end it
                    } else {
                        IrValue *av = e->as.else_expr.arm ? ir_lower_expr(c, e->as.else_expr.arm) : NULL;
                        if (e->as.else_expr.is_panic) ir_set_unreachable(c->cur);
                        else { if (av) ir_store(c->f, c->cur, cell2, av); ir_set_br(c->cur, jn2); }
                    }
                    c->cur = jn2;
                    return ir_load(c->f, c->cur, cell2, rt2);
                }
            }
            IrValue *uv = ir_lower_expr(c, opx);
            if (!uv || !uv->type || uv->type->kind != IRT_SUM)
                return ir_opaque_expr(c, ty, false, "try-else-non-union", NULL, NULL);
            // Variant 0 is the payload variant, and sema wraps the value in a one-field
            // struct (`__payload { __v: T }`), so the type of the projection is that field's,
            // not the variant's.
            IrType *vt0 = uv->type->n_fields > 0 ? uv->type->fields[0] : NULL;
            IrType *pty = (vt0 && vt0->kind==IRT_STRUCT && vt0->n_fields > 0) ? vt0->fields[0] : vt0;
            if (!pty) return ir_opaque_expr(c, ty, false, "try-else-no-payload", NULL, NULL);

            IrValue *tag  = ir_sum_tag(c->f, c->cur, uv);
            IrValue *zero = ir_const_int(c->f, c->cur, 0, tag->type);
            IrValue *isok = ir_icmp(c->f, c->cur, IR_CMP_EQ, tag, zero);

            IrValue *cell = ir_alloca(c->f, c->cur, pty);
            IrBlock *okb = ir_new_block(c->f), *bad = ir_new_block(c->f), *jn = ir_new_block(c->f);
            ir_set_br_cond(c->cur, isok, okb, bad);

            c->cur = okb;                                  // the value: project variant 0
            ir_store(c->f, c->cur, cell, ir_sum_payload(c->f, c->cur, uv, 0, 0, pty));
            ir_set_br(c->cur, jn);

            c->cur = bad;
            if (is_try) {
                // Propagating a marker LEAVES THE FUNCTION, so it must run the pending defers
                // exactly as an ordinary `return` does — `defer { cleanup() }` before a failing
                // `try` was simply skipped.
                ir_lower_flush_defers(c);
                IrType *rt = c->f->ret_type;
                if (ir_is_set_term(c->cur)) {
                    // a deferred panic or [noreturn] call ended the path: nothing is returned
                } else if (rt && rt->kind==IRT_SUM && rt != uv->type) {
                    // WIDENING. `try` inside a function returning a wider union must RE-ENCODE
                    // the marker: `*u8 | NotFound` propagating into `*u8 | NotFound | ParseErr`
                    // is the same marker at a different variant index. Returning the narrow sum
                    // unchanged is a type error in the emitted C and a lie in the IR. The tag
                    // is dynamic, so the mapping is a chain of tag tests — one per marker the
                    // narrow union has, matched to the wide union BY NAME.
                    IrValue *tg = ir_sum_tag(c->f, c->cur, uv);
                    for (int k = 1; k < uv->type->n_fields; k++) {
                        IrName *mn = uv->type->field_names[k];
                        int w = -1;
                        for (int j = 1; j < rt->n_fields && mn; j++) {
                            IrName *wn = rt->field_names[j];
                            if (wn && wn->length==mn->length
                                && strncmp(wn->name, mn->name, (size_t)mn->length)==0) { w = j; break; }
                        }
                        if (w < 0) continue;
                        IrBlock *hit = ir_new_block(c->f), *nxt = ir_new_block(c->f);
                        IrValue *kc = ir_const_int(c->f, c->cur, k, tg->type);
                        ir_set_br_cond(c->cur, ir_icmp(c->f,c->cur,IR_CMP_EQ,tg,kc), hit, nxt);
                        c->cur = hit;
                        ir_set_ret(c->cur, ir_sum_new(c->f, c->cur, rt, w, NULL, 0));
                        c->cur = nxt;
                    }
                    ir_set_unreachable(c->cur);     // the tag was a marker: one arm always hits
                } else {
                    ir_set_ret(c->cur, uv);                // same union: propagate unchanged
                }
            } else if (e->as.else_expr.arm_is_return) {
                IrValue *rv = e->as.else_expr.arm ? ir_lower_expr(c, e->as.else_expr.arm) : NULL;
                ir_lower_flush_defers(c);
                if (!ir_is_set_term(c->cur)) ir_set_ret(c->cur, rv);   // a defer may end it
            } else {
                IrValue *av = e->as.else_expr.arm ? ir_lower_expr(c, e->as.else_expr.arm) : NULL;
                if (e->as.else_expr.is_panic) {
                    ir_set_unreachable(c->cur);            // `panic` never yields a value
                } else {
                    if (av) ir_store(c->f, c->cur, cell, av);
                    ir_set_br(c->cur, jn);
                }
            }
            c->cur = jn;
            return ir_load(c->f, c->cur, cell, pty);
        }
        // A case EXPRESSION — `return case o { Some(v): v  None: d }`. Only the case
        // STATEMENT was lowered, so every one of these became an OPAQUE unknown and the
        // function returned garbage: the whole generics/Option family plus match_advanced.
        // Same shape as the statement form (a tag/value test chain), differing only in that
        // each arm is ONE expression whose value is stored into a result cell.
        case EXPR_MATCH: {
            Expr *val = e->as.match_expr.value;
            IrType *sumty = NULL;
            bool vty_kind_is_scalar = false;
            IrType *vty = ir_lower_type(c, val ? val->type : NULL);
            bool string_ok = ir_is_u8_string_type(vty);
            { if (vty && vty->kind == IRT_SUM) sumty = vty;
              vty_kind_is_scalar = vty && (vty->kind == IRT_INT || vty->kind == IRT_BOOL); }
            // ★ ASK THE LOWERED TYPE, NOT THE AST SHAPE. This read
            // `vt->kind==TYPE_SIMPLE && vt->int_width_cache>0`, so a `bool` scrutinee — which is
            // not an integer and has no width — fell through to an OPAQUE placeholder, suppressing
            // every proof over the whole function. It was the last `incomplete` function in the
            // corpus (`tests/match/bool_exhaustive_pass.ln`, spec §15.4: `true`/`false` arms are
            // exhaustive with no `else`), and the arm path below needs nothing special for it — a
            // scalar arm is an `IR_CMP_EQ`, which a bool supports.
            //
            // Asking the IR type is also the right question for lowering to ask: what it can match
            // on is a property of the IR, and re-deriving it from AST fields is how the two drift.
            bool scalar_ok = vty_kind_is_scalar;
            if (!sumty && !scalar_ok && !string_ok)
                return ir_opaque_expr(c, ty, false, "a `case` on a scrutinee of this type", NULL, NULL);
            IrValue *v = ir_lower_expr(c, val);
            if (sumty && !(v && v->type && v->type->kind==IRT_SUM))
                return ir_opaque_expr(c, ty, false, "a `case` on a scrutinee of this type", NULL, NULL);
            IrStrScrut ss = {0};
            if (string_ok) ir_string_scrutinee(c, v, vty, &ss);



            IrType *rty = ty && ty->kind!=IRT_UNIT ? ty : ir_type_int(c->a,32,true);
            IrValue *cell = ir_alloca(c->f, c->cur, rty);
            IrValue *tagv = sumty ? ir_sum_tag(c->f, c->cur, v) : NULL;
            IrBlock *join = ir_new_block(c->f);
            ExprMatchCase *elsec = NULL;
            for (ExprMatchCase *cs = e->as.match_expr.cases; cs; cs = cs->next) {
                if (!cs->patterns) { elsec = cs; continue; }
                IrBlock *body = ir_new_block(c->f);
                int bound_k = -1; Expr *bound_pat = NULL;
                for (ExprList *p = cs->patterns; p; p = p->next) {
                    Expr *pe = p->expr;
                    IrBlock *nxt = ir_new_block(c->f);
                    if (sumty) {
                        Expr *pv = (pe->kind==EXPR_CALL) ? pe->as.call_expr.callee : pe;
                        Decl *ed = NULL;
                        if (sumty->sname) { Id tn; tn.name=sumty->sname->name; tn.length=sumty->sname->length;
                                            ed = ir_find_enum_decl(c, &tn); }
                        int k = ed ? ir_variant_index(ed, ir_variant_name_of(pv), true) : -1;
                        if (k < 0) {   // the front end refuses this (E106); never match everything
                            if (pe->line) { ir_cur_line = pe->line; ir_cur_col = pe->col; }
                            ir_incomplete(c, "a pattern that names no variant of the scrutinee"); ir_set_br(c->cur, body); c->cur = nxt; continue;
                        }
                        if (pe->kind==EXPR_CALL) { bound_k = k; bound_pat = pe; }
                        IrValue *kc = ir_const_int(c->f, c->cur, k, ir_type_int(c->a,32,true));
                        ir_match_branch(c, sumty, k, pe, v,
                                        ir_icmp(c->f,c->cur,IR_CMP_EQ,tagv,kc), body, nxt);
                    } else if (string_ok) {   // only string literals here (E012 in the front end)
                        if (pe->kind == EXPR_STRING) ir_match_string(c, &ss, pe, body, nxt);
                        else { ir_incomplete(c, "a pattern that is not a string literal"); ir_set_br(c->cur, nxt); }
                    } else if (pe->kind == EXPR_RANGE) {
                        Expr *loe=pe->as.range_expr.start, *hie=pe->as.range_expr.end;
                        IrBlock *hitest = ir_new_block(c->f);
                        if (loe) ir_set_br_cond(c->cur, ir_icmp(c->f,c->cur,IR_CMP_SGE,v,ir_lower_expr(c,loe)), hitest, nxt);
                        else     ir_set_br(c->cur, hitest);
                        c->cur = hitest;
                        if (hie) { IrCmp cc2 = pe->as.range_expr.inclusive?IR_CMP_SLE:IR_CMP_SLT;
                                   ir_set_br_cond(c->cur, ir_icmp(c->f,c->cur,cc2,v,ir_lower_expr(c,hie)), body, nxt); }
                        else     ir_set_br(c->cur, body);
                    } else {
                        ir_set_br_cond(c->cur, ir_icmp(c->f,c->cur,IR_CMP_EQ,v,ir_lower_expr(c,pe)), body, nxt);
                    }
                    c->cur = nxt;
                }
                IrBlock *ftblk = c->cur;
                c->cur = body;
                IrLocal *saved = c->locals;
                if (bound_k >= 0 && bound_pat) {
                    IrType *pl = (bound_k < sumty->n_fields) ? sumty->fields[bound_k] : NULL;
                    int j=0;
                    for (ExprList *a = bound_pat->as.call_expr.args; a; a = a->next, j++) {
                        Id *bn = a->expr && a->expr->kind==EXPR_IDENTIFIER
                               ? a->expr->as.identifier_expr.id : NULL;
                        if (!bn || !pl || j >= pl->n_fields) continue;
                        // a nested variant NAME tested the payload; binding it here would
                        // shadow the variant with the value it was testing for
                        if (ir_nested_variant_index(c, pl->fields[j], a->expr) >= 0) continue;
                        ir_env_add(c, bn, NULL, ir_sum_payload(c->f, c->cur, v, bound_k, j, pl->fields[j]));
                    }
                }
                if (cs->body) { IrValue *av = ir_lower_expr(c, cs->body);
                                if (av) ir_store(c->f, c->cur, cell, av); }
                c->locals = saved;
                if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, join);
                c->cur = ftblk;
            }
            if (elsec && elsec->body) {
                IrValue *av = ir_lower_expr(c, elsec->body);
                if (av) ir_store(c->f, c->cur, cell, av);
                if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, join);
            } else if (!ir_is_set_term(c->cur)) {
                // No `else` arm. A case EXPRESSION has to yield a value, so the front end
                // requires it to be exhaustive and this fall-through cannot be taken. Say
                // UNREACHABLE rather than branch to the join without storing: that path
                // reaches the load with the cell unwritten, and definite-assignment was right
                // to call it a read of an uninitialised value (10 false positives). Inventing
                // a default here would be worse — it would make an unreachable path look
                // defined instead of saying it does not exist.
                ir_set_unreachable(c->cur);
            }
            c->cur = join;
            return ir_load(c->f, c->cur, cell, rty);
        }
        case EXPR_FLOAT_LITERAL:
            return ir_const_float(c->f, c->cur, e->as.float_expr.value,
                                  ty && ty->kind==IRT_FLOAT ? ty : ir_type_float(c->a, 64));
        case EXPR_LITERAL: {
            // A literal's type comes from sema, which leaves a bare integer at i32 even where
            // the context is wider — `var x i64 = 5000000000` typed the literal i32 and the
            // constant was TRUNCATED to 705032704. The old engine avoids it by pasting the
            // literal straight into a C initializer and letting the C compiler pick the type;
            // the IR materialises a typed value, so it has to be honest about the width.
            // Refusing to encode a value the type cannot hold is the minimum: widen instead.
            IrType *lt = ty;
            int64_t lv = e->as.literal_expr.value;
            int64_t llo, lhi;
            if (lt && irtype_int_range(lt, &llo, &lhi) && (lv < llo || lv > lhi))
                lt = ir_type_int(c->a, 64, lv < 0 || (lt && lt->is_signed));
            return ir_const_int(c->f, c->cur, lv, lt);
        }
        case EXPR_CHAR:
            return ir_const_int(c->f, c->cur, e->as.char_expr.value, ty);
        case EXPR_STRING: {
            IrValue *data = ir_str_const(c->f, c->cur, e->as.string_expr.value,
                                         (int32_t)e->as.string_expr.length);
            if (ty && ty->kind == IRT_SLICE) {   // u8[:0] context → a fat {data,len}
                IrValue *ln = ir_const_int(c->f, c->cur, e->as.string_expr.length,
                                           ir_type_int(c->a,64,false));
                return ir_make_slice(c->f, c->cur, data, ln, ty->elem);
            }
            return data;   // fixed u8[N:0] / pointer context: the data pointer
        }
        case EXPR_IDENTIFIER: {
            // ★ AN UNDECLARED NAME MAKES THIS FUNCTION UNJUDGEABLE, and saying so is the whole
            // point. E106 is raised by the EMITTER — after resolution, monomorphization and
            // UFCS, which is what makes its predicate reliable — so it runs LATER than the
            // analyses. With the numeric obligations authoritative, `return x + missing` was
            // reported as "arithmetic is not provably free of overflow": a confusing message
            // about the wrong thing, and the program never reached the emitter that knew.
            //
            // The predicate is the emitter's, verbatim. E106 is now raised before lowering
            // (sema_check_undeclared, main.c); this is the backstop, and an incomplete function
            // is refused at the name.
            if (e->line > 0 && e->type == NULL && e->decl == NULL && !e->is_global &&
                !ir_env_find(c, e->as.identifier_expr.id))
                ir_incomplete(c, "undeclared-identifier");
            IrLocal *l = ir_env_find(c, e->as.identifier_expr.id);
            if (l && l->param) return l->param;
            if (l && l->aggregate) return l->slot;   // array/slice base pointer, read directly
            { IrType *rp = ir_ref_binding_ptr(l);    // a reference binding reads its REFERENT
              if (rp) return ir_load(c->f, c->cur, ir_load(c->f, c->cur, l->slot, rp), rp->elem); }
            if (l && l->slot)  return ir_load(c->f, c->cur, l->slot,
                                              l->slot->type->elem ? l->slot->type->elem : ty);
            // A bare variant (`return NotFound`). resolve.h rewrites the identifier to
            // `<mod>_<Enum>_<Variant>`, points e->decl at the ENUM, and records the variant.
            if (e->decl && e->decl->kind == DECL_ENUM) {
                int k = ir_variant_index(e->decl, ir_variant_name_of(e), true);
                if (k >= 0) {
                    Id *en = e->decl->as.enum_decl.type_name;
                    IrType *st = (ty && ty->kind==IRT_SUM) ? ty : NULL;
                    if (!st && en) { Type tt; memset(&tt,0,sizeof tt);
                                     tt.kind = TYPE_SIMPLE; tt.base_type = en;
                                     st = ir_lower_type(c, &tt); }
                    if (st && st->kind==IRT_SUM) return ir_sum_new(c->f, c->cur, st, k, NULL, 0);
                }
            }
            // A function NAME used as a VALUE — `var f *func(i32,i32) i32 = choose`. It is
            // neither a local nor a constant, and with no case here it became an OPAQUE
            // unknown, so the pointer was never actually stored.
            if (e->decl && (e->decl->kind==DECL_FUNCTION
                         || e->decl->kind==DECL_EXTERN_FUNCTION)) {
                Id *fnm = e->decl->as.function_decl.name;
                // The SAME identity the definition and every call site use — a function
                // referenced as a value is the same function, and a second naming scheme here
                // would emit a name nothing declares. Four programs did exactly that.
                // ★ THE ARROW'S SHAPE COMES FROM THE DECLARATION, NOT FROM A FALLBACK.
                // The old code used the expression's type when it happened to be IRT_FUNC and
                // otherwise built an EMPTY one — no return type, no parameters — which emits
                // as `void (*)(void)` and then does not accept the function being assigned to
                // it. A function type with no signature is not a conservative approximation;
                // it is a different type, and C says so.
                //
                // The declaration is right here and has both halves. `ir_lower_type` on each
                // is the same lowering every other type goes through.
                // ...ALWAYS from the declaration. The expression's own type was preferred when it
                // was a function type, and a function NAME is typed by its RETURN type, so a
                // function that returns a function pointer (`func pick() *func(i32) i32`) was
                // referenced as if it WERE that pointer: `pick()` emitted `v1 = pick; v2 = v1();`
                // with v1 declared `int32_t (*)(int32_t)`, and gcc refused the call.
                IrType *fty = NULL;
                if (!fty) {
                    fty = ir_type_new(c->a, IRT_FUNC);
                    fty->elem = ir_lower_type(c, e->decl->as.function_decl.return_type);
                    int np = 0;
                    for (DeclList *q = e->decl->as.function_decl.params; q; q = q->next) np++;
                    if (np > 0) {
                        fty->fields = arena_push_many_aligned(c->a, IrType*, np);
                        int qi = 0;
                        for (DeclList *q = e->decl->as.function_decl.params; q; q = q->next, qi++)
                            fty->fields[qi] = (q->decl && q->decl->kind==DECL_VARIABLE)
                                            ? ir_lower_type(c, q->decl->as.variable_decl.type)
                                            : ir_type_int(c->a, 32, true);
                        fty->n_fields = np;
                    }
                    // The arrow's effect bound is the referenced function's own ROW: what it
                    // may do is what a caller through the pointer may see. `proc` (deprecated)
                    // grants everything, which is what its keyword always meant.
                    { bool isf = (e->decl->kind==DECL_FUNCTION || e->decl->kind==DECL_EXTERN_FUNCTION);
                      EffectSet r = e->decl->as.function_decl.effects_declared
                                  ? e->decl->as.function_decl.effects_bound : 0;
                      if (!isf) r |= EFFECT_TOP;
                      IrEffect ir = 0;
                      if (r & EFFECT_DIVERGE) ir |= IR_EFFECT_DIVERGE;
                      if (r & EFFECT_RAISES)  ir |= IR_EFFECT_RAISES;
                      if (r & EFFECT_IO)      ir |= IR_EFFECT_IO;
                      if (r & EFFECT_ALLOC)   ir |= IR_EFFECT_ALLOC;
                      fty->fn_row = ir; }
                }
                if (fnm) return ir_func_ref(c->f, c->cur,
                                            ir_qualified_name(c->a, e->decl, fnm), fty);
            }
            // not a local/param: a module-level constant folds to its initializer
            if (c->const_depth < 32) {
                // ★ THE DECLARATION THE RESOLVER BOUND, before any name match (I.98). A top-level
                // constant is registered under `<compiling module>_<name>`, so inside an IMPORTED
                // module's function `K` arrives here as `use_K` while K's declaration belongs to
                // `lib`, no name matched, and every module-level constant was unreadable from the
                // module that defines it once that module was imported (Handwriting: Marco's
                // ctype.ln table, read by is_space, could not be imported into lexer.ln).
                Decl *g = (e->is_global && e->decl && e->decl->kind == DECL_VARIABLE &&
                           e->decl->as.variable_decl.init && !e->decl->as.variable_decl.is_mutable)
                          ? e->decl : ir_find_global_const(c, e->as.identifier_expr.id);
                // A global ARRAY cannot fold to a scalar, and leaving it OPAQUE meant every
                // read of one dereferenced null: four programs SEGFAULTED. Materialise it —
                // an immutable global with a literal initializer is exactly a local array
                // with the same initializer, and the copy is once per reference in a function
                // rather than the static the old backend emits. Correct first; the storage
                // is a backend concern (4.1) and the IR does not need to name it.
                if (g && g->as.variable_decl.init
                    && g->as.variable_decl.init->kind == EXPR_ARRAY_LITERAL
                    && ty && ty->kind == IRT_ARRAY) {
                    // ★ A TABLE IS READ-ONLY STATIC DATA, not a copy made at every use. Built as
                    // a local, `CTYPE u8[256]` read in `is_space(c)` compiled at gcc -O2 to 16
                    // vector loads from .rodata and 16 stores to the stack on EVERY call, and a
                    // lexer calls it per character. An integer table whose every element is a
                    // constant is one `static const` object; the alloca that names it keeps it
                    // an array of known length, fully initialised, to every analysis.
                    // ...only where it is READ through an index. Anywhere else (an argument, a
                    // copy, a slice) the use keeps its own copy: an array parameter is an output
                    // reference the callee may write, and .rodata cannot be written.
                    IrData *dt = (c->table_read == e) ? ir_data_for_global(c, g, ty) : NULL;
                    if (dt) {
                        IrValue *agg = ir_data_array(c->f, c->cur, dt);
                        ir_init_fact(c->f, c->cur, agg);
                        return agg;
                    }
                    IrSealMark sm = ir_seal_open(c);      // a module constant: immutable storage
                    IrValue *agg = ir_alloca_array(c->f, c->cur, ty);
                    int k = 0;
                    c->const_depth++;
                    for (ExprList *el = g->as.variable_decl.init->as.array_literal_expr.elements;
                         el; el = el->next, k++) {
                        IrValue *idx = ir_const_int(c->f, c->cur, k, ir_type_int(c->a,64,false));
                        IrValue *p   = ir_elem_ptr(c->f, c->cur, agg, idx, ty->elem);
                        ir_store(c->f, c->cur, p, ir_lower_expr(c, el->expr));
                    }
                    c->const_depth--;
                    ir_init_fact(c->f, c->cur, agg);      // every element is written
                    ir_seal_close(c, sm);
                    return agg;
                }
                if (g) { c->const_depth++;
                         IrValue *v = ir_lower_expr(c, g->as.variable_decl.init);
                         c->const_depth--;
                         // ★ THE CONSTANT HAS ITS DECLARED TYPE at every use. The initialiser was
                         // returned in its OWN type: `X u8 = 1 << 9` was computed as an int32_t
                         // and used as 512 — X was never a u8 anywhere. A module constant lands in
                         // no slot, so this is its slot: a CHECKED conversion to the declared type,
                         // which the range analysis must discharge (E086) like any other narrowing.
                         // The front end checks a value it can evaluate (5c99f19); this covers the
                         // ones it cannot.
                         if (v && v->type && ty && v->type->kind == IRT_INT && ty->kind == IRT_INT &&
                             (v->type->bits != ty->bits || v->type->is_signed != ty->is_signed)) {
                             IrInstr *cv = ir_instr(c->f, IR_CAST, ty, 1);
                             cv->operands[0] = v;
                             cv->aux.cast_kind = v->type->bits > ty->bits ? IR_CAST_TRUNC
                                               : v->type->bits < ty->bits ? (v->type->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT)
                                               : IR_CAST_BITCAST;
                             cv->wrap = IR_WRAP_CHECK;
                             ir_emit(c->cur, cv);
                             v = cv->result;
                         }
                         return v; }
            }
            // A global we cannot fold (typically a global ARRAY). `const 0` was a LIE — the
            // value is simply wrong, and any proof built on it is built on a fiction. An
            // opaque READ says only what is true: the value is unknown.
            return ir_opaque_expr(c, ty, false, "unresolved-global", NULL, NULL);
        }
        case EXPR_BINARY: {
            Expr *L=e->as.binary_expr.left, *R=e->as.binary_expr.right;
            // `i in container` — a valid-index guard: 0 ≤ i < container.len. Lowered to
            // `i < len` (the ≥ 0 half comes from i's type/flow); this makes it a real
            // icmp so guard refinement and the termination check both engage.
            if (e->as.binary_expr.op == TOKEN_KEYWORD_IN && R && R->kind == EXPR_ARRAY_LITERAL) {
                IrValue *x = ir_lower_expr(c, L);
                IrType *bt = ir_type_bool(c->a);
                IrValue *rcell = ir_alloca(c->f, c->cur, bt);
                IrBlock *yes = ir_new_block(c->f), *no = ir_new_block(c->f), *jn = ir_new_block(c->f);
                ir_list_member_br(c, R, x, yes, no);
                c->cur = yes; ir_store(c->f, c->cur, rcell, ir_const_int(c->f,c->cur,1,bt)); ir_set_br(c->cur, jn);
                c->cur = no;  ir_store(c->f, c->cur, rcell, ir_const_int(c->f,c->cur,0,bt)); ir_set_br(c->cur, jn);
                c->cur = jn;  return ir_load(c->f, c->cur, rcell, bt);
            }
            if (e->as.binary_expr.op == TOKEN_KEYWORD_IN && R && R->kind == EXPR_RANGE) {
                // One test suffices when the lower one cannot fail (`0..` on an unsigned x): then the
                // value is that comparison, exactly `x < hi`, and needs no cell (I.78).
                IrValue *x = ir_lower_expr(c, L);
                Expr *lo = R->as.range_expr.start, *hi = R->as.range_expr.end;
                if (lo && hi && lo->kind == EXPR_LITERAL && lo->as.literal_expr.value == 0 &&
                    x->type && x->type->kind == IRT_INT && !x->type->is_signed)
                    return ir_cmp_value(c, R->as.range_expr.inclusive ? TOKEN_ANGLE_BRACKET_LEFT_EQUAL
                                                                      : TOKEN_ANGLE_BRACKET_LEFT,
                                        x, ir_lower_expr(c, hi), false, ir_nonneg_lit(hi), false);
                IrType *bt = ir_type_bool(c->a);
                IrValue *rcell = ir_alloca(c->f, c->cur, bt);
                IrBlock *yes = ir_new_block(c->f), *no = ir_new_block(c->f), *jn = ir_new_block(c->f);
                ir_range_member_br(c, R, x, yes, no);
                c->cur = yes; ir_store(c->f, c->cur, rcell, ir_const_int(c->f,c->cur,1,bt)); ir_set_br(c->cur, jn);
                c->cur = no;  ir_store(c->f, c->cur, rcell, ir_const_int(c->f,c->cur,0,bt)); ir_set_br(c->cur, jn);
                c->cur = jn;  return ir_load(c->f, c->cur, rcell, bt);
            }
            if (e->as.binary_expr.op == TOKEN_KEYWORD_IN) {
                IrValue *a = ir_lower_expr(c, L);
                IrValue *len;
                if (R->type && R->type->kind==TYPE_ARRAY && R->type->array_len>=0)
                    len = ir_const_int(c->f, c->cur, R->type->array_len, ir_type_int(c->a,64,false));
                else len = ir_slice_len(c->f, c->cur, ir_lower_expr(c, R));
                return ir_icmp(c->f, c->cur, IR_CMP_ULT, a, len);
            }
            // `s == "hi"` on SLICES is a structural comparison, not an integer one — and
            // IR_ICMP is defined on integers, so emitting one produced C that compares two
            // structs with `==`. It is the one place the language's `==` means something other
            // than "same scalar".
            if (e->as.binary_expr.op==TOKEN_EQUAL_EQUAL || e->as.binary_expr.op==TOKEN_BANG_EQUAL) {
                IrType *lt = L ? ir_lower_type(c, L->type) : NULL;
                IrType *rt = R ? ir_lower_type(c, R->type) : NULL;
                if (lt && rt && lt->kind==IRT_SLICE && rt->kind==IRT_SLICE) {
                    IrType *bt = ir_type_bool(c->a);
                    IrValue *eq = ir_seq_eq(c->f, c->cur, ir_lower_expr(c,L), ir_lower_expr(c,R), bt);
                    if (e->as.binary_expr.op==TOKEN_BANG_EQUAL)
                        eq = ir_icmp(c->f, c->cur, IR_CMP_EQ, eq, ir_const_int(c->f,c->cur,0,bt));
                    return eq;
                }
            }
            // SATURATING `+| -| *|` — clamp to the type's interval instead of overflowing.
            // Not lowered at all before, so `250 +| 100` on a u8 became an OPAQUE and printed
            // 0. Expanded here rather than left as a wrap MODE the backend would have to
            // interpret: written out, the numeric domain gets the result's exact interval for
            // free (a saturating result is in [lo,hi] by construction) and no analysis needs
            // to know the operator exists.
            if (e->as.binary_expr.op==TOKEN_PLUS_PIPE || e->as.binary_expr.op==TOKEN_MINUS_PIPE
             || e->as.binary_expr.op==TOKEN_ASTERISK_PIPE) {
                IrType *rt3 = ir_lower_type(c, e->type);
                int64_t slo, shi;
                if (rt3 && rt3->kind==IRT_INT && irtype_int_range(rt3,&slo,&shi) &&
                    ir_widen_serves(rt3, e->as.binary_expr.op)) {
                    IrType *w = ir_type_int(c->a, 64, true);
                    IrCastKind ck = rt3->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT;
                    IrValue *la = ir_lower_expr(c, L), *ra = ir_lower_expr(c, R);
                    IrInstr *cl = ir_instr(c->f, IR_CAST, w, 1); cl->operands[0]=la;
                        cl->aux.cast_kind=ck; ir_emit(c->cur, cl);
                    IrInstr *cr = ir_instr(c->f, IR_CAST, w, 1); cr->operands[0]=ra;
                        cr->aux.cast_kind=ck; ir_emit(c->cur, cr);
                    IrOp op = e->as.binary_expr.op==TOKEN_PLUS_PIPE  ? IR_ADD
                            : e->as.binary_expr.op==TOKEN_MINUS_PIPE ? IR_SUB : IR_MUL;
                    IrValue *wide = ir_binop(c->f, c->cur, op, cl->result, cr->result, w);
                    IrValue *cell = ir_alloca(c->f, c->cur, rt3);
                    IrBlock *hib = ir_new_block(c->f), *lotest = ir_new_block(c->f),
                            *lob = ir_new_block(c->f), *okb3 = ir_new_block(c->f),
                            *jn2 = ir_new_block(c->f);
                    ir_set_br_cond(c->cur,                                     // above the top?
                        ir_icmp(c->f,c->cur,IR_CMP_SGT,wide,ir_const_int(c->f,c->cur,shi,w)), hib, lotest);
                    c->cur = hib;    ir_store(c->f,c->cur,cell,ir_const_int(c->f,c->cur,shi,rt3));
                                     ir_set_br(c->cur, jn2);
                    c->cur = lotest; ir_set_br_cond(c->cur,                    // below the bottom?
                        ir_icmp(c->f,c->cur,IR_CMP_SLT,wide,ir_const_int(c->f,c->cur,slo,w)), lob, okb3);
                    c->cur = lob;    ir_store(c->f,c->cur,cell,ir_const_int(c->f,c->cur,slo,rt3));
                                     ir_set_br(c->cur, jn2);
                    c->cur = okb3;                                             // in range: the value
                    { IrInstr *nr = ir_instr(c->f, IR_CAST, rt3, 1); nr->operands[0]=wide;
                      nr->aux.cast_kind=IR_CAST_TRUNC; ir_emit(c->cur, nr);
                      ir_store(c->f, c->cur, cell, nr->result); }
                    ir_set_br(c->cur, jn2);
                    c->cur = jn2;
                    return ir_load(c->f, c->cur, cell, rt3);
                }
                if (rt3 && rt3->kind==IRT_INT && irtype_int_range(rt3,&slo,&shi) && rt3->bits<=64) {
                    IrValue *la = ir_lower_expr(c, L), *ra = ir_lower_expr(c, R);
                    return ir_lower_guarded(c, e->as.binary_expr.op, la, ra, rt3, slo, shi, NULL);
                }
            }
            // Short-circuit `and` / `or`: the right operand must NOT be evaluated when
            // the left already decides the result (correctness — it may guard a deref/
            // index), so this needs real control flow, not a bitwise op. The br_cond on
            // the left's condition also lets the octagon refine the eval-right block.
            if (e->as.binary_expr.op==TOKEN_KEYWORD_AND || e->as.binary_expr.op==TOKEN_KEYWORD_OR) {
                bool is_and = (e->as.binary_expr.op==TOKEN_KEYWORD_AND);
                IrType *bt = ir_type_bool(c->a);
                IrValue *rcell = ir_alloca(c->f, c->cur, bt);
                IrValue *xa = ir_lower_expr(c, L);
                IrBlock *evb=ir_new_block(c->f), *sk=ir_new_block(c->f), *jn=ir_new_block(c->f);
                if (is_and) ir_set_br_cond(c->cur, xa, evb, sk);   // and: eval R only if L true
                else        ir_set_br_cond(c->cur, xa, sk, evb);   // or:  eval R only if L false
                c->cur = evb; ir_store(c->f, c->cur, rcell, ir_lower_expr(c, R)); ir_set_br(c->cur, jn);
                c->cur = sk;  ir_store(c->f, c->cur, rcell, ir_const_int(c->f,c->cur,is_and?0:1,bt)); ir_set_br(c->cur, jn);
                c->cur = jn;  return ir_load(c->f, c->cur, rcell, bt);
            }
            // Signedness comes from the OPERANDS, not e->type: a comparison's result
            // type is bool, so deriving from it would mistag every `i < len` as signed.
            Type *sty = (L->type && L->type->kind==TYPE_SIMPLE && L->type->int_width_cache>0) ? L->type
                      : (R->type && R->type->kind==TYPE_SIMPLE && R->type->int_width_cache>0) ? R->type
                      : e->type;
            // An operator with an overflow policy computes in ITS type, which a literal on the left
            // takes from the other operand (I.56): `100 /% x` on a u64 is an unsigned division.
            {
                TokenKind po = e->as.binary_expr.op;
                bool policy = po == TOKEN_PLUS_PERCENT || po == TOKEN_MINUS_PERCENT || po == TOKEN_ASTERISK_PERCENT ||
                              po == TOKEN_SLASH_PERCENT || po == TOKEN_PLUS_PIPE || po == TOKEN_MINUS_PIPE ||
                              po == TOKEN_ASTERISK_PIPE || po == TOKEN_SLASH_PIPE || po == TOKEN_PLUS_QUESTION ||
                              po == TOKEN_MINUS_QUESTION || po == TOKEN_ASTERISK_QUESTION;
                if (policy && e->type && e->type->kind==TYPE_SIMPLE && e->type->int_width_cache>0) sty = e->type;
            }
            bool sgn = !(sty && sty->kind==TYPE_SIMPLE && sty->int_width_cache>0 && !sty->int_signed_cache);
            // An ARRAY LITERAL beside a vector (`a - [1, 1, 1, 1]`) is that vector: it types as
            // an array, so it was materialised as one and its BASE POINTER met the vector —
            // C that gcc refuses (`vector - pointer`), or, once a scalar operand was cast to the
            // lane type, a pointer broadcast into every lane. Built lane by lane instead.
            IrType *lvt = L->type ? ir_lower_type(c, L->type) : NULL;
            IrType *rvt = R->type ? ir_lower_type(c, R->type) : NULL;
            IrValue *x = (L->kind==EXPR_ARRAY_LITERAL && rvt && rvt->kind==IRT_VECTOR)
                       ? ir_lower_vec_literal(c, L, rvt) : NULL;
            if (!x) x = ir_lower_expr(c, L);
            IrValue *y = (R->kind==EXPR_ARRAY_LITERAL && lvt && lvt->kind==IRT_VECTOR)
                       ? ir_lower_vec_literal(c, R, lvt) : NULL;
            if (!y) y = ir_lower_expr(c, R);
            IrOp op; IrWrapMode wrap; IrCmp cmp;
            if (ir_cmp_op(e->as.binary_expr.op, sgn, &cmp))
                return ir_cmp_value(c, e->as.binary_expr.op, x, y, ir_nonneg_lit(L), ir_nonneg_lit(R), sgn);
            if (ir_bin_op(e->as.binary_expr.op, sgn, &op, &wrap)) {
                // DECIDE-M: a SIGNED remainder never overflows mathematically, but C's `%` is
                // undefined at TYPE_MIN % -1 (because TYPE_MIN / -1 is). Below 64 bits it is
                // computed in i64, where that case is an ordinary 0, and narrowed back — exact,
                // since |a % b| < |b|. At 64 bits there is no wider type and the op keeps the
                // obligation (vra_check_divzero).
                if (op == IR_SREM && wrap == IR_WRAP_CHECK && ty && ty->kind==IRT_INT &&
                    ty->is_signed && ty->bits < 64 && x && y && x->type && y->type &&
                    x->type->kind==IRT_INT && y->type->kind==IRT_INT) {
                    IrType *w64 = ir_type_int(c->a, 64, true);
                    IrValue *xw = x, *yw = y;
                    if (x->type->bits < 64) { IrInstr *cx = ir_instr(c->f, IR_CAST, w64, 1); cx->operands[0] = x;
                        cx->aux.cast_kind = x->type->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT; ir_emit(c->cur, cx); xw = cx->result; }
                    if (y->type->bits < 64) { IrInstr *cy = ir_instr(c->f, IR_CAST, w64, 1); cy->operands[0] = y;
                        cy->aux.cast_kind = y->type->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT; ir_emit(c->cur, cy); yw = cy->result; }
                    IrValue *rw = ir_binop(c->f, c->cur, IR_SREM, xw, yw, w64);
                    c->cur->instrs_tail->unchecked = c->unsafe;
                    IrInstr *ct = ir_instr(c->f, IR_CAST, ty, 1); ct->operands[0] = rw;
                    ct->aux.cast_kind = IR_CAST_TRUNC; ir_emit(c->cur, ct);
                    return ct->result;
                }
                IrValue *r = ir_binop(c->f,c->cur,op,x,y,ty);
                c->cur->instrs_tail->wrap = wrap;
                // `unchecked` is documented on IR_ELEM_PTR *and arithmetic* — but only the
                // element pointer ever set it, so `unsafe { var c u8 = a + b }` still carried
                // an overflow obligation the corpus test says it must not. Inside `unsafe` the
                // programmer has taken responsibility; the IR has to record that it happened.
                c->cur->instrs_tail->unchecked = c->unsafe;
                return r;
            }
            // an unhandled binary operator: a PURE computation over two known values —
            // unknown result, no memory effect.
            return ir_opaque_expr(c, ty, false, "unhandled-binop", x, y);
        }
        case EXPR_UNARY: {
            IrValue *x = ir_lower_expr(c, e->as.unary_expr.right);
            if (e->as.unary_expr.op == TOKEN_MINUS) { IrInstr *ins=ir_instr(c->f,IR_NEG,ty,1); ins->operands[0]=x; ir_emit(c->cur,ins); return ins->result; }
            // `!x` is LOGICAL not — `x == 0` — and `~x` is bitwise. Both lowered to IR_BNOT, which
            // the backend emits as `~`: on a C `_Bool` holding 1 that is -2, which is TRUE, so
            // `var b bool = true; if !b { … }` took the branch, and so did `!f()` and `!(x == 5)`.
            // A silent miscompile of every negated boolean, in the shipping compiler until
            // 2026-09-28 (the corpus's `!` uses happened to sit where the value did not matter).
            if (e->as.unary_expr.op == TOKEN_BANG && x && x->type)
                return ir_icmp(c->f, c->cur, IR_CMP_EQ, x, ir_const_int(c->f, c->cur, 0, x->type));
            IrInstr *ins=ir_instr(c->f,IR_BNOT,ty,1); ins->operands[0]=x; ir_emit(c->cur,ins); return ins->result;
        }
        case EXPR_INDEX: {
            Expr *idxe = e->as.index_expr.index;
            if (idxe && idxe->kind == EXPR_RANGE) {   // subslice: xs[lo..hi] → a slice value
                IrType *u64t  = ir_type_int(c->a,64,false);
                // A module constant table's slice is a view of its read-only static object, as
                // `T[i]` is: a slice taken from a per-use copy died with the frame, so returning
                // `T[0..2]` was E010. A write through the view is E009 (borrow.h).
                Expr *saved_tr = c->table_read;
                c->table_read = e->as.index_expr.target;
                IrValue *tv   = ir_lower_expr(c, e->as.index_expr.target);
                c->table_read = saved_tr;
                IrType  *selem= (ty && ty->elem) ? ty->elem : ir_type_int(c->a,8,false);
                bool src_slice= tv->type && tv->type->kind==IRT_SLICE;
                IrValue *srclen = src_slice ? ir_slice_len(c->f,c->cur,tv)
                    : (e->as.index_expr.target->type && e->as.index_expr.target->type->kind==TYPE_ARRAY
                        ? ir_const_int(c->f,c->cur, e->as.index_expr.target->type->array_len, u64t) : NULL);
                IrValue *data0= src_slice ? ir_slice_data(c->f,c->cur,tv,selem) : tv;
                IrValue *lo   = idxe->as.range_expr.start ? ir_lower_expr(c, idxe->as.range_expr.start)
                                                          : ir_const_int(c->f,c->cur,0,u64t);
                IrValue *hi   = idxe->as.range_expr.end   ? ir_lower_expr(c, idxe->as.range_expr.end)
                              : (srclen ? srclen : ir_const_int(c->f,c->cur,0,u64t));
                IrValue *nd   = ir_elem_ptr(c->f,c->cur, data0, lo, selem);
                IrValue *len  = ir_binop(c->f,c->cur, IR_SUB, hi, lo, u64t);
                if (idxe->as.range_expr.inclusive)
                    len = ir_binop(c->f,c->cur, IR_ADD, len, ir_const_int(c->f,c->cur,1,u64t), u64t);
                return ir_make_slice(c->f,c->cur, nd, len, selem);
            }
            Expr *saved_tr = c->table_read;
            c->table_read = e->as.index_expr.target;          // `T[i]` READS T
            IrValue *addr = ir_lower_addr(c, e);
            c->table_read = saved_tr;
            return ir_load(c->f, c->cur, addr, ty);
        }
        case EXPR_ARRAY_LITERAL: {
            // An array literal OUTSIDE a variable initialiser — `M([10,20,30,40], 4)`. It was
            // `unhandled-expr`, so the constructor got an OPAQUE null and the struct's array
            // field held garbage. There is no array VALUE in this model, so materialise it
            // into a slot and hand back the decayed base, exactly like an array local.
            if (ty && ty->kind==IRT_ARRAY) {
                IrValue *agg = ir_alloca_array(c->f, c->cur, ty);
                int k = 0;
                for (ExprList *el = e->as.array_literal_expr.elements; el; el = el->next, k++) {
                    IrValue *idx = ir_const_int(c->f, c->cur, k, ir_type_int(c->a,64,false));
                    IrValue *p   = ir_elem_ptr(c->f, c->cur, agg, idx, ty->elem);
                    ir_store(c->f, c->cur, p, ir_lower_expr(c, el->expr));
                }
                ir_init_fact(c->f, c->cur, agg);
                return agg;
            }
            // Not an array-typed literal (a vector, say): fall to the same placeholder the
            // default case uses. A bare `break` here runs off the end of a non-void function —
            // the exact undefined behaviour `check_build_warnings.sh` exists for, and the
            // THIRD time this switch has grown that shape.
            return ir_opaque_expr(c, ty, true, "unhandled-expr", NULL, NULL);
        }
        case EXPR_MEMBER: {
            Id *m = e->as.member_expr.member;
            Expr *tgt = e->as.member_expr.target;
            Type *tst = tgt ? tgt->type : NULL;
            // `Shape.Point` — a payload-less variant. Checked before the field path because a
            // variant reference has no struct base at all (its target types as UNIT), which is
            // why these arrived at the field lookup and became the top `incomplete` cause.
            if (ty && ty->kind==IRT_SUM) {
                Decl *ed = (e->decl && e->decl->kind==DECL_ENUM) ? e->decl
                         : ir_find_enum_decl(c, tgt && tgt->kind==EXPR_IDENTIFIER
                                                 ? tgt->as.identifier_expr.id : NULL);
                if (!ed && ty->sname) { Id tn; tn.name = ty->sname->name; tn.length = ty->sname->length;
                                        ed = ir_find_enum_decl(c, &tn); }
                int k = ed ? ir_variant_index(ed, m, true) : -1;
                if (k >= 0) return ir_sum_new(c->f, c->cur, ty, k, NULL, 0);
            }
            // `.data` — the slice's base pointer. Only `.len` was handled, so every
            // `s.data` fell through to the struct-field path, failed to resolve (a slice is
            // not a struct) and became an OPAQUE unknown: `libc_printf("%s", greeting().data)`
            // printed "(null)". It is the idiom the whole C boundary is written in.
            if (m && m->length==4 && strncmp(m->name,"data",4)==0) {
                IrValue *sv = ir_lower_expr(c, tgt);
                if (sv && sv->type && sv->type->kind==IRT_SLICE)
                    return ir_slice_data(c->f, c->cur, sv,
                                         ty && ty->kind==IRT_PTR && ty->elem ? ty->elem : sv->type->elem);
                if (sv && sv->type && sv->type->kind==IRT_ARRAY) return sv;   // already a base
                // (fall through: a struct field literally named `data`)
            }
            if (m && m->length==3 && strncmp(m->name,"len",3)==0) {
                Type *ust = sema_unwrap_type(tst);                    // an alias of an array type
                if (ust && ust->kind==TYPE_ARRAY && ust->array_len>=0)
                    return ir_const_int(c->f, c->cur, ust->array_len, ty);   // fixed array .len = N
                // otherwise decide by the LOWERED target's IR type — robust to untyped
                // refinement/size exprs (e.g. `src.len - 1` in `i32[src.len - 1]`).
                IrValue *s = ir_lower_expr(c, tgt);
                if (s && s->type && s->type->kind==IRT_SLICE) return ir_slice_len(c->f, c->cur, s);
                if (s && s->type && s->type->kind==IRT_ARRAY)
                    return ir_const_int(c->f, c->cur, s->type->array_len, ty);
                // (fall through: a struct field literally named `len`)
            }
            // ── `s.Variant.field` — reading a payload OUTSIDE a `case` arm ──────────────────
            // The inner member names a VARIANT, not a field, so `ir_field_index` on the sum
            // returned -1 and the whole expression became `opaque.unchecked` — honest, and
            // useless: the value was never computed, and the new backend emitted a program
            // that printed 0 where the old one printed 10 (`emit_gate`'s `unsafe_adt_pass`).
            //
            // Everything needed already existed — `ir_sum_payload`, the `aux.sum` addressing,
            // the backend's case for it — used only from the `case`-arm path. The only missing
            // piece was this SOURCE FORM. Discrimination is the programmer's responsibility
            // here (the language requires `unsafe` for exactly that reason, and IR_SUM_PAYLOAD
            // is well-defined only where the tag is known), so lowering states the projection
            // and adds no check of its own.
            if (tgt && tgt->kind == EXPR_MEMBER) {
                Expr *sumex = tgt->as.member_expr.target;
                IrType *sumty = sumex ? ir_struct_of(ir_lower_type(c, sumex->type)) : NULL;
                if (sumty && sumty->kind == IRT_SUM) {
                    Id *vn = tgt->as.member_expr.member;
                    int k = -1;
                    for (int q = 0; q < sumty->n_fields; q++) {
                        IrName *n2 = sumty->field_names[q];
                        if (n2 && vn && n2->length == vn->length
                            && memcmp(n2->name, vn->name, (size_t)vn->length) == 0) { k = q; break; }
                    }
                    if (k >= 0) {
                        IrType *pl = sumty->fields[k];
                        int fj = -1; IrType *pfty = NULL;
                        if (pl && pl->kind == IRT_STRUCT)
                            for (int q = 0; q < pl->n_fields; q++) {
                                IrName *n3 = pl->field_names[q];
                                if (n3 && m && n3->length == m->length
                                    && memcmp(n3->name, m->name, (size_t)m->length) == 0) {
                                    fj = q; pfty = pl->fields[q]; break; }
                            }
                        if (fj >= 0) {
                            IrValue *sv = ir_lower_expr(c, sumex);
                            return ir_sum_payload(c->f, c->cur, sv, k, fj, pfty ? pfty : ty);
                        }
                    }
                }
            }
            // struct field read: load through the field address
            IrType *sty = ir_struct_of(ir_lower_type(c, tst));
            IrType *fty = NULL;
            int fidx = ir_field_index(sty, m, &fty);
            if (fidx >= 0) {
                IrValue *addr = ir_lower_addr(c, e);
                // An ARRAY field READS as its decayed base, not as a load of the whole array —
                // there is no array VALUE in this model, and indexing one produced C that
                // subscripted a pointer read out of the array's own bytes.
                if (fty && fty->kind==IRT_ARRAY && addr && addr->type
                    && addr->type->kind==IRT_ARRAY) return addr;
                IrValue *v = ir_load(c->f, c->cur, addr, fty ? fty : ty);
                ir_field_len_bound_fact(c, sty, fidx, v, false);      // a slice's length bound, consumed
                // ...and a RELATIONAL field invariant, consumed the same way.
                { IrFieldRel rels[8];
                  int nr = (v->type && v->type->kind==IRT_INT) ? ir_field_relations(c, sty, fidx, rels, 8) : 0;
                  IrValue *base = nr ? ir_lower_addr(c, tgt) : NULL;
                  for (int q = 0; base && q < nr; q++) {
                      IrValue *rv = ir_field_rel_rhs_at(c, base, sty, &rels[q], v->type);
                      if (rv) ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, rels[q].cmp, v, rv));
                  } }
                return v;
            }
            // a field we could not resolve: a READ of unknown storage, no write.
            return ir_opaque_expr(c, ty, false, "unresolved-member", NULL, NULL);
        }
        case EXPR_CALL: {
            Decl *callee = e->as.call_expr.callee ? e->as.call_expr.callee->decl : NULL;
            // an INDIRECT call (through a fn-pointer VARIABLE) has a non-function callee decl:
            // null it so we don't read `.function_decl` off a variable_decl (a wrong-union
            // crash). A DECL_STRUCT callee is a constructor, handled just below — leave it.
            // A call through a fn-pointer VARIABLE names no function. It used to null the
            // callee decl and then fall back to the identifier's name, so the emitted C called
            // a function that does not exist; the analyses, meanwhile, resolved that name to
            // nothing and silently treated the call as unknown — right answer, wrong reason.
            // Detected from the callee's TYPE, not its decl: a local fn-pointer has no decl
            // attached at the call site, so keying on DECL_VARIABLE missed every real case.
            Expr *cx = e->as.call_expr.callee;
            // A NAME bound to a function is a direct call whatever its type says: its type is the
            // return type, a function pointer for `pick() *func(i32) i32`, which made `pick()` an
            // indirect call through a value of the wrong type. A callee that is itself a call,
            // `pick()(5)`, has a function pointer as its value and stays indirect.
            bool names_fn = cx && cx->kind == EXPR_IDENTIFIER && callee &&
                            (callee->kind == DECL_FUNCTION || callee->kind == DECL_EXTERN_FUNCTION);
            bool indirect = !names_fn &&
                            ((callee && callee->kind==DECL_VARIABLE)
                             || (cx && cx->type && cx->type->kind==TYPE_FUNC));
            if (indirect) callee = NULL;
            int n=0; for (ExprList *a=e->as.call_expr.args; a; a=a->next) n++;
            // `Point(1, 2)` is struct construction, not a call: build a struct value
            // from the positional field args (in declaration order).
            if (callee && callee->kind == DECL_STRUCT && ty && ty->kind==IRT_STRUCT) {
                IrValue **fs = arena_push_many_aligned(c->a, IrValue*, n>0?n:1);
                // Each argument takes its FIELD's representation. This was the one boundary
                // ir_coerce_repr was written for and never applied at: `W(buf, 7)` with `buf
                // u8[4]` and `s u8[]` built `(W){ buf, 7 }`, and C's brace elision filled
                // s.data = buf, s.len = 7, k = 0 — every later field shifted by one word, in
                // an accepted program. The call path decays an array; this path passed the
                // pointer through.
                int k=0; for (ExprList *a=e->as.call_expr.args; a; a=a->next,k++)
                    fs[k] = ir_coerce_repr(c, ir_lower_expr(c, a->expr),
                                           k < ty->n_fields ? ty->fields[k] : NULL, a->expr);
                // ...and so is a slice field's length bound.
                for (int fi=0; fi<n && fi<ty->n_fields; fi++) ir_field_len_bound_fact(c, ty, fi, fs[fi], true);
                // A RELATIONAL field invariant is established here too, from the operands.
                for (int fi=0; fi<n && fi<ty->n_fields; fi++) {
                    IrFieldRel rels[8];
                    int nr = ir_field_relations(c, ty, fi, rels, 8);
                    for (int q = 0; q < nr; q++) {
                        int oj = rels[q].other;
                        if (oj >= n || !fs[fi] || !fs[oj]) continue;
                        IrValue *rv = ir_field_rel_rhs(c, fs[oj], ty->fields[oj], rels[q].is_len, fs[fi]->type);
                        if (rv) ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, rels[q].cmp, fs[fi], rv), 121);
                    }
                }
                return ir_struct_new(c->f, c->cur, ty, fs, n);
            }
            // `Shape.Circle(10)` is VARIANT construction, not a call. The callee decl is the
            // ENUM (sema resolves the member to its owning type), so the variant comes from
            // the member name.
            if (callee && callee->kind == DECL_ENUM && ty && ty->kind==IRT_SUM) {
                int k = ir_variant_index(callee, ir_variant_name_of(e->as.call_expr.callee), true);
                if (k >= 0) {
                    IrValue **fs = arena_push_many_aligned(c->a, IrValue*, n>0?n:1);
                    // the variant's payload types: an IRT_STRUCT for a multi-field payload,
                    // the field type itself for a single one (see IRT_SUM's layout in ir.h)
                    IrType *pay = (k < ty->n_fields) ? ty->fields[k] : NULL;
                    int j=0; for (ExprList *a=e->as.call_expr.args; a; a=a->next,j++) {
                        fs[j] = ir_lower_expr(c, a->expr);
                        IrType *want = NULL;
                        if (pay && pay->kind==IRT_STRUCT && j < pay->n_fields) want = pay->fields[j];
                        else if (pay && n==1) want = pay;
                        fs[j] = ir_coerce_repr(c, fs[j], want, a->expr);
                    }
                    return ir_sum_new(c->f, c->cur, ty, k, fs, n);
                }
            }
            IrValue *fnv = indirect ? ir_lower_expr(c, e->as.call_expr.callee) : NULL;
            // What a call PRODUCES is the callee's signature, not the call site's inferred
            // type. Reading only the latter lost the result of every INFERRED generic call:
            // monomorphization rewrites `map(s, triple)` to name the instance but leaves the
            // expression's own type unresolved, so the call was emitted with no result and a
            // `const 0` placeholder took its place — `Option_i32 = int32_t` in the C. The
            // callee is the authority; the expression type is the fallback for a callee we
            // cannot resolve (indirect calls, declless builtins).
            IrType *cret = NULL;
            if (callee && (callee->kind==DECL_FUNCTION
                        || callee->kind==DECL_EXTERN_FUNCTION))
                cret = ir_lower_borrow_binding_type(c, callee->as.function_decl.return_type);
            IrType *rty = ((e->type && e->type->kind!=TYPE_SIMPLE) || (ty->kind!=IRT_UNIT)) ? ty
                        : (cret && cret->kind!=IRT_UNIT ? cret : NULL);
            // A call to a function returning a BORROW yields a pointer, and the generic
            // expression type above does not know that — `ty` came from ir_lower_type, which
            // deliberately does not apply the borrow rule. Prefer the callee's binding type
            // whenever it says pointer: that is the signature speaking, and the signature is
            // what the callee will actually return.
            if (cret && cret->kind==IRT_PTR && (!rty || rty->kind!=IRT_PTR)) rty = cret;
            IrInstr *ins = ir_instr(c->f, IR_CALL, rty, indirect ? n + 1 : n);
            Id *cnm = callee ? callee->as.function_decl.name : NULL;   // intern the callee name
            if (!indirect && !cnm && e->as.call_expr.callee && e->as.call_expr.callee->kind==EXPR_IDENTIFIER)
                cnm = e->as.call_expr.callee->as.identifier_expr.id;   // declless builtin (e.g. `panic`)
            // ★ THE CALL SITE MUST NAME THE CALLEE THE SAME WAY THE DEFINITION DOES, or every
            // lookup misses — and a MISS is worse than a wrong hit here, because an
            // unattributable call is treated as opaque and the analyses fall back to the worst
            // case. `callee` is the resolved Decl, so its `defining_module` is the right one
            // even for a qualified call across modules, which is the whole point of D-54.
            //
            // The declless-builtin fallback (`panic`) has no Decl and no module: it qualifies
            // to itself, which matches how it is DEFINED — the emitter writes it into the
            // preamble under the bare name.
            ins->aux.callee = (!indirect && cnm) ? ir_qualified_name(c->a, callee, cnm) : NULL;
            if (indirect) ins->operands[0] = fnv;
            DeclList *pp = callee ? callee->as.function_decl.params : NULL;
            int i=0;
            if (indirect) i = 1;                       // operand 0 is the callee value
            for (ExprList *a=e->as.call_expr.args; a; a=a->next,i++) {
                { int k = indirect ? i - 1 : i;          // which parameter this argument binds
                  if (a->expr && a->expr->ro_root && k >= 0 && k < 64) ins->ro_args |= (uint64_t)1 << k; }
                // ── A SHARED BORROW OF AN AGGREGATE IS PASSED BY ADDRESS ────────────────
                // The callee's parameter is a pointer (see the param loop and
                // `ir_shared_agg_by_address`), so the argument has to be one. Taken BEFORE
                // lowering the expression as a value, because `ir_lower_addr` handles the
                // rvalue case itself — `f(mk())` materialises the temporary into a slot and
                // addresses that, which is the ordinary C rule for a temporary's lifetime.
                //
                // Only where the callee is KNOWN: an indirect call names no function, so its
                // convention cannot be read, and it keeps the by-value shape on both sides.
                if (pp && pp->decl && pp->decl->kind==DECL_VARIABLE && !indirect) {
                    Type   *pdt = pp->decl->as.variable_decl.type;
                    IrType *plt = ir_lower_type(c, pdt);
                    if (ir_shared_agg_by_address(plt, pdt) && a->expr) {
                        // ★ ONLY A PLACE HAS AN ADDRESS. `name(Color.Green)` passes an RVALUE —
                        // a freshly constructed sum — and `ir_lower_addr` answers a
                        // non-addressable expression with an OPAQUE pointer, which is honest
                        // and fatal: three programs segfaulted on a pointer nothing had ever
                        // pointed anywhere. A variant reference is an `EXPR_MEMBER` like a
                        // field access and has to be told apart by its DECL, not its shape.
                        //
                        // Anything that is not a place is materialised into a slot and the
                        // slot's address is passed — the ordinary C rule for a temporary's
                        // lifetime, and exactly what the parameter loop does for a by-value
                        // struct. It costs the copy this change exists to remove, but only
                        // where the value had to be built anyway.
                        Expr *ax = a->expr;
                        bool is_variant = (ax->kind==EXPR_MEMBER && ax->decl
                                           && ax->decl->kind==DECL_ENUM);
                        bool place = !is_variant &&
                                     (ax->kind==EXPR_IDENTIFIER || ax->kind==EXPR_DEREF
                                      || ax->kind==EXPR_INDEX   || ax->kind==EXPR_MEMBER);
                        IrValue *addr = NULL;
                        if (place) {
                            addr = ir_lower_addr(c, ax);
                            if (!addr || !addr->type || addr->type->kind!=IRT_PTR) addr = NULL;
                        }
                        if (!addr) {                       // an rvalue: give it storage
                            IrValue *tv = ir_lower_expr(c, ax);
                            if (tv && tv->type && (tv->type->kind==IRT_STRUCT || tv->type->kind==IRT_SUM)) {
                                IrValue *slot = ir_alloca(c->f, c->cur, tv->type);
                                ir_store(c->f, c->cur, slot, tv);
                                addr = slot;
                            } else {
                                ins->operands[i] = tv;      // not an aggregate after all
                                pp = pp->next; continue;
                            }
                        }
                        ins->operands[i] = addr;
                        pp = pp->next;
                        continue;
                    }
                }
                IrValue *av = ir_lower_expr(c, a->expr);
                // a fixed array decays to a slice where the callee expects one, and a slice to its
                // data pointer where a `*T` is declared (`libc_printf("x")`) — ir_coerce_repr,
                // the relation every boundary shares.
                if (pp && pp->decl && pp->decl->kind==DECL_VARIABLE) {
                    IrType *ptype = ir_lower_type(c, pp->decl->as.variable_decl.type);
                    av = ir_coerce_repr(c, av, ptype, a->expr);
                }
                ins->operands[i] = av;
                if (pp) pp = pp->next;
            }
            ir_lower_call_requires(c, callee, ins);   // contracts: prove the callee's scalar preconditions
            ir_lower_call_slice_len_requires(c, callee, ins);  // …and its sized-slice length preconditions
            if (!indirect)                                     // …and its constant-length arrays
                ir_lower_call_fixed_len_requires(c, callee, ins, e->as.call_expr.args);
            ir_emit(c->cur, ins);
            if (ins->result) ir_lower_return_ensures(c, callee, ins->result, ins);  // contracts: learn the ensures
            return ins->result ? ins->result : ir_const_int(c->f,c->cur,0,ir_type_int(c->a,32,true));
        }
        case EXPR_CAST: {
            IrValue *x = ir_lower_expr(c, e->as.cast_expr.expr);
            IrType  *dt = ir_lower_type(c, e->as.cast_expr.target_type);
            IrInstr *ins = ir_instr(c->f, IR_CAST, dt, 1);
            ins->operands[0]=x;
            // ★ The cast KIND, from the types and the tier — every source-level `as` used to
            // be labelled BITCAST regardless.
            //
            // Two things went wrong with one label. The narrowing obligation fires only on
            // IR_CAST_TRUNC, so `500 as u8` silently produced 244 — while
            // spec/chapters/08-expressions.tex says of the proven tier: "narrows only if VRA
            // proves it fits, else E086". The compiler was not implementing its own
            // specification, and the whole promise of the language is that a narrowing
            // cannot silently lose information. And in the other direction a widening was
            // labelled BITCAST too, so nothing downstream could tell that it preserves its
            // operand's value (see vra_range, which now decides that from the types).
            //
            // Only the PROVEN tier owes the obligation. `as%` truncates modularly and `as|`
            // clamps: both are total, and asking them to prove they fit would be asking them
            // to prove they are unnecessary.
            //
            // ★ THE TIER TRAVELS ON THE CAST'S WRAP MODE, and the kind is only the widths. The
            // tier used to survive as nothing but "not TRUNC", so nothing downstream could tell
            // the policies apart: `as|` was emitted as the plain C conversion and did not clamp
            // (300 as| u8 was 44), `as%` to an odd width did not wrap (300 as% u4 was 44), and
            // the range analysis copied the SOURCE value through all three. And the PROVEN tier
            // owed its obligation only when the width shrank, so a signedness change —
            // `4294967295 as i32`, `u8 200 as i8` — reinterpreted silently, though spec 07 makes
            // it a narrowing like any other. The obligation now follows the VALUES (any cast that
            // may lose one, vra_check_narrow), not the kind.
            IrType *st = x ? x->type : NULL;
            CastKind tier = e->as.cast_expr.kind;
            if (st && dt && st->kind==IRT_INT && dt->kind==IRT_INT) {
                if (st->bits > dt->bits)      ins->aux.cast_kind = IR_CAST_TRUNC;
                else if (st->bits < dt->bits) ins->aux.cast_kind = st->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT;
                else                          ins->aux.cast_kind = IR_CAST_BITCAST;   // same width
                ins->wrap = tier == CAST_WRAPPING   ? IR_WRAP_MODULAR
                          : tier == CAST_SATURATING ? IR_WRAP_SATURATE : IR_WRAP_CHECK;
            } else {
                ins->aux.cast_kind = IR_CAST_BITCAST;
            }
            ir_emit(c->cur,ins);
            return ins->result;
        }
        case EXPR_MOVE: {                               // `mov x`: read the value, then INVALIDATE
            Expr *src = e->as.move_expr.expr;            // the source PLACE (linearity)
            IrValue *v = ir_lower_expr(c, src);
            if (src && src->kind==EXPR_IDENTIFIER) {
                IrLocal *l = ir_env_find(c, src->as.identifier_expr.id);
                // `mov r` on a REFERENCE BINDING moves what r refers to — the consume must name
                // that place, not r's slot. Naming the slot marked the binding "moved" while the
                // owner stayed live, so `var r = var t.a; rfree(mov r); split(mov t)` freed t.a
                // twice. (The linearity pass then refuses the move itself: r is a borrow, E020.)
                IrType *rp = ir_ref_binding_ptr(l);
                if (rp) ir_consume(c->f, c->cur, ir_load(c->f, c->cur, l->slot, rp));
                else if (l && l->slot) ir_consume(c->f, c->cur, l->slot);   // slot is now moved-from
            } else if (src && (src->kind==EXPR_MEMBER || src->kind==EXPR_INDEX)) {
                // `mov r.handle` consumes a FIELD, and emitting nothing for it lost the fact
                // entirely — the struct then looked unconsumed at every return. A move
                // consumes the PLACE it names, whatever its depth; the place lattice is what
                // relates `r.handle` back to `r` (per-field linear state, design §2).
                IrValue *a = ir_lower_addr(c, src);
                if (a) ir_consume(c->f, c->cur, a);
            }
            return v;
        }
        case EXPR_MUT: {                                // `var lv`: a mutable borrow
            Expr *inner = e->as.mut_expr.expr;
            IrType *it = inner ? ir_lower_type(c, inner->type) : NULL;
            // A local that is already a borrow (`var r = var p.x`, `var r = pick(var a, var b)`)
            // holds the pointer; `use_ref(var r)` passes it, not the address of r's slot.
            // ir_lower_addr does that for every reference binding (ir_ref_binding_ptr).
            // a COPIED type (struct or scalar) must travel as its ADDRESS or the callee's
            // writes are lost; a slice/array/ptr already shares its data, so pass the value.
            if (ir_mut_by_address(it)) return ir_lower_addr(c, inner);
            return ir_lower_expr(c, inner);
        }
        case EXPR_ADDR:                                 // &lvalue
            return ir_lower_addr(c, e->as.addr_expr.expr);
        case EXPR_DEREF: {                              // *ptr
            IrValue *p = ir_lower_expr(c, e->as.deref_expr.expr);
            return ir_load(c->f, c->cur, p, ty);
        }
        case EXPR_BUILTIN: {
            // The SCALAR bit intrinsics are ordinary integer ops; the SIMD ones (@load/@store/
            // @splat/@movemask/@shuffle) lower below, on IRT_VECTOR.
            BuiltinKind bk = e->as.builtin_expr.builtin_kind;
            if (bk==BUILTIN_CTZ || bk==BUILTIN_CLZ || bk==BUILTIN_POPCOUNT) {
                IrValue *x = ir_lower_expr(c, e->as.builtin_expr.arg);
                IrOp op = bk==BUILTIN_CTZ ? IR_CTZ : bk==BUILTIN_CLZ ? IR_CLZ : IR_POPCOUNT;
                return ir_bitcount(c->f, c->cur, op, x, ty && ty->kind==IRT_INT ? ty
                                                       : ir_type_int(c->a,32,false));
            }
            // `@likely(x)` / `@unlikely(x)` are branch HINTS with the value of their
            // argument — semantically the identity, so lowering them away is faithful.
            if (bk==BUILTIN_LIKELY || bk==BUILTIN_UNLIKELY)
                return ir_lower_expr(c, e->as.builtin_expr.arg);
            if (bk==BUILTIN_SIZEOF || bk==BUILTIN_ALIGNOF) {
                IrType *qt = ir_lower_type(c, e->as.builtin_expr.vec_type);
                if (!qt || qt->kind == IRT_UNIT)
                    return ir_opaque_expr(c, ty, false, "sizeof-of-unknown-type", NULL, NULL);
                return ir_sizeof(c->f, c->cur, qt, bk==BUILTIN_ALIGNOF);
            }
            // Fall through to the same placeholder the default case uses. A bare `break`
            // here exited the switch and ran off the end of a non-void function — undefined
            // behaviour that returned a garbage IrValue*, which the VRA then dereferenced
            // (a segfault on any @movemask/@load/@store/@shuffle program). gcc DID warn
            // ("control reaches end of non-void function"); the build output was being
            // grepped for `error` only.
            // SIMD, now that IRT_VECTOR exists. @load and @store are ordinary memory
            // accesses that happen to be WIDE — which is the whole point of recording the
            // width on the element pointer: the bounds obligation for a 32-lane load at `i`
            // is `i + 32 <= len`, and stating it that way means the existing numeric analysis
            // proves or refuses it with no SIMD-specific reasoning at all.
            if (bk==BUILTIN_LOAD || bk==BUILTIN_STORE) {
                Type *vt = e->as.builtin_expr.vec_type;
                IrType *vecty = (bk==BUILTIN_LOAD) ? ir_lower_type(c, vt) : NULL;
                IrValue *basev = ir_lower_expr(c, e->as.builtin_expr.arg);
                IrValue *offv  = e->as.builtin_expr.arg2 ? ir_lower_expr(c, e->as.builtin_expr.arg2)
                                                         : ir_const_int(c->f,c->cur,0,ir_type_int(c->a,64,false));
                IrValue *val = NULL;
                if (bk==BUILTIN_STORE) {
                    val = e->as.builtin_expr.arg3 ? ir_lower_expr(c, e->as.builtin_expr.arg3) : NULL;
                    vecty = val ? val->type : NULL;
                }
                if (vecty && vecty->kind==IRT_VECTOR && basev) {
                    IrType *lane = vecty->elem ? vecty->elem : ir_type_int(c->a,8,false);
                    if (basev->type && basev->type->kind==IRT_SLICE)
                        basev = ir_slice_data(c->f, c->cur, basev, lane);
                    IrValue *p = ir_elem_ptr_wide(c->f, c->cur, basev, offv, lane, (int)vecty->array_len);
                    p->line = e->line; p->col = e->col;
                    if (bk==BUILTIN_LOAD) return ir_load(c->f, c->cur, p, vecty);
                    if (val) ir_store(c->f, c->cur, p, val);
                    return ir_const_int(c->f, c->cur, 0, ir_type_int(c->a,32,true));
                }
            }
            if (bk==BUILTIN_SPLAT) {
                IrType *vecty = ir_lower_type(c, e->as.builtin_expr.vec_type);
                IrValue *x = e->as.builtin_expr.arg ? ir_lower_expr(c, e->as.builtin_expr.arg) : NULL;
                if (vecty && vecty->kind==IRT_VECTOR && x) {
                    int n = (int)vecty->array_len;
                    IrValue **lanes = arena_push_many_aligned(c->a, IrValue*, n > 0 ? n : 1);
                    for (int k=0;k<n;k++) lanes[k] = x;      // every lane is the same value
                    return ir_struct_new(c->f, c->cur, vecty, lanes, n);
                }
            }
            if (bk==BUILTIN_MOVEMASK) {
                IrValue *v = e->as.builtin_expr.arg ? ir_lower_expr(c, e->as.builtin_expr.arg) : NULL;
                if (v && v->type && v->type->kind==IRT_VECTOR)
                    return ir_vec_movemask(c->f, c->cur, v,
                                           ty && ty->kind==IRT_INT ? ty : ir_type_int(c->a,32,false));
            }
            if (bk==BUILTIN_SHUFFLE) {
                IrValue *tv = e->as.builtin_expr.arg  ? ir_lower_expr(c, e->as.builtin_expr.arg)  : NULL;
                IrValue *iv = e->as.builtin_expr.arg2 ? ir_lower_expr(c, e->as.builtin_expr.arg2) : NULL;
                if (tv && iv && tv->type && tv->type->kind==IRT_VECTOR &&
                    iv->type && iv->type->kind==IRT_VECTOR) {
                    IrValue *r = ir_vec_shuffle(c->f, c->cur, tv, iv);
                    // An index spelled `e & K`, K a constant below N, is in range in every lane:
                    // an unsigned `& K` is at most K. That is the classifier idiom
                    // (`(x >> 4) & 15`), and gcc keeps the zeroing's three instructions next to
                    // pshufb unless told. Per-lane ranges in the analysis would generalise this.
                    Expr *ie = e->as.builtin_expr.arg2;
                    if (ie && ie->kind == EXPR_BINARY && ie->as.binary_expr.op == TOKEN_AMPERSAND) {
                        Expr *kx = ie->as.binary_expr.right->kind == EXPR_LITERAL ? ie->as.binary_expr.right
                                 : ie->as.binary_expr.left->kind  == EXPR_LITERAL ? ie->as.binary_expr.left : NULL;
                        if (kx && kx->as.literal_expr.value >= 0 &&
                            kx->as.literal_expr.value < (int64_t)tv->type->array_len)
                            c->cur->instrs_tail->aux.imm = 1;
                    }
                    return r;
                }
            }
            // Anything else here is unmodelled. A @store WRITES memory, so declare the footprint.
            return ir_opaque_expr(c, ty, true, "simd-builtin",
                                  e->as.builtin_expr.arg ? ir_lower_expr(c, e->as.builtin_expr.arg) : NULL, NULL);
        }
        default:
            // Unknown shape ⇒ assume the worst footprint (it may write).
            return ir_opaque_expr(c, ty, true, "unhandled-expr", NULL, NULL);
    }
}

// Lower a boolean condition as a branch cur→tb (true) / cur→fb (false), expanding
// short-circuit `and`/`or` into NESTED guards. This keeps the octagon refining BOTH
// operands along the taken path — a materialized bool cell (the expression lowering of
// `A and B`) hides the numeric refinement from the analysis, so `if a<n and b<n {arr[a]}`
// wouldn't prove. (Guard context only; the expression form still uses the bool cell.)
// An OPTIONAL-SHAPED sum: exactly one variant with a payload and exactly one without.
// `*u8 | none`, Rust's `Option<T>`, Zig's `?T`, a nullable pointer — all the same shape, which
// is why this is a property of the TYPE and not a Lain spelling. Returns false for anything
// else (a three-way union is not an optional and has no truth value).
static bool ir_sum_optional_shape(IrType *t, int *some_k, int *none_k) {
    if (!t || t->kind!=IRT_SUM || t->n_fields!=2) return false;
    int s=-1, n=-1;
    for (int i=0;i<2;i++) { if (t->fields[i]) { if (s>=0) return false; s=i; } else { if (n>=0) return false; n=i; } }
    if (s<0 || n<0) return false;
    *some_k = s; *none_k = n; return true;
}
// The payload of an optional sum, as the narrowed value `if x` makes available.
static IrValue *ir_sum_optional_payload(LowerCtx *c, IrValue *v, int some_k) {
    IrType *pay = v->type->fields[some_k];
    if (pay && pay->kind==IRT_STRUCT && pay->n_fields==1)      // single-field payloads are
        return ir_sum_payload(c->f, c->cur, v, some_k, 0, pay->fields[0]);   // wrapped in a struct
    return ir_sum_payload(c->f, c->cur, v, some_k, 0, pay);
}
// Lower an expression used as a TRUTH VALUE. A sum has no truth value of its own; an OPTIONAL
// one does — `if x` asks whether it is the payload variant. Without this the branch tested the
// whole struct, which is not even well-typed in C.
static IrValue *ir_lower_truth(LowerCtx *c, Expr *cond) {
    IrValue *v = ir_lower_expr(c, cond);
    int sk, nk;
    if (v && v->type && ir_sum_optional_shape(v->type, &sk, &nk)) {
        IrValue *tag = ir_sum_tag(c->f, c->cur, v);
        IrValue *nc  = ir_const_int(c->f, c->cur, nk, tag->type);
        return ir_icmp(c->f, c->cur, IR_CMP_NE, tag, nc);
    }
    return v;
}

// `x in lo..hi` / `x in lo..=hi` (DECIDE-X, I.77): x, lo and hi are each evaluated once, in that
// order, and the test is `lo <= x` then `x < hi` (`<=` for `..=`), two comparisons on two branches,
// so guard refinement narrows x on each exactly as it does for the `and` spelled out. Each is an
// ordinary comparison (I.88: mathematical, whatever the operands' signedness). `0 <= x` on an
// unsigned x always holds and is not emitted, so `i in 0..n` is exactly `i < n`.
static void ir_lower_range_member(LowerCtx *c, Expr *e, IrBlock *tb, IrBlock *fb) {
    ir_range_member_br(c, e->as.binary_expr.right, ir_lower_expr(c, e->as.binary_expr.left), tb, fb);
}
// The tests of `x in R` with x already lowered (it is evaluated exactly once, first).
static void ir_range_member_br(LowerCtx *c, Expr *R, IrValue *x, IrBlock *tb, IrBlock *fb) {
    Expr *lo = R->as.range_expr.start, *hi = R->as.range_expr.end;
    bool lo_trivial = lo && lo->kind == EXPR_LITERAL && lo->as.literal_expr.value == 0 &&
                      x->type && x->type->kind == IRT_INT && !x->type->is_signed;
    IrValue *lv = (lo && !lo_trivial) ? ir_lower_expr(c, lo) : NULL;   // a literal: no effect to keep
    IrValue *hv = hi ? ir_lower_expr(c, hi) : NULL;
    // The upper test follows in the SAME block when the lower one is skipped: a loop's header must
    // end in its test, or it has no exit test at all (I.85) and `while i in 0..n` is refused.
    if (lv) {
        IrBlock *hitest = ir_new_block(c->f);
        ir_cmp_br(c, TOKEN_ANGLE_BRACKET_LEFT_EQUAL, lv, x, ir_nonneg_lit(lo), false, true, hitest, fb);
        c->cur = hitest;
    }
    // ★ PAST A LOWER BOUND THAT IS A NON-NEGATIVE LITERAL, x >= 0 (I.78). A non-negative value
    // converts exactly to any unsigned type, so against an unsigned upper bound ONE unsigned compare
    // is exact in the IR, the interpreter and C: no sign test. That test was not only redundant, it
    // hid the bound from termination: its block keeps both branches inside a loop, so it is not an
    // exit test (I.85) and the chain stopped before the real one. `while (i + 15) in 0..src.len`
    // (the guard computed widened, so signed) was E011 in the SIMD lexer. The retired `i in a`
    // lowered to exactly this unsigned compare.
    bool x_nonneg = lo_trivial || (lo && ir_nonneg_lit(lo));
    if (hv && x_nonneg && x->type && x->type->kind == IRT_INT && x->type->is_signed &&
        hv->type && hv->type->kind == IRT_INT && !hv->type->is_signed) {
        ir_set_br_cond(c->cur, ir_icmp(c->f, c->cur, R->as.range_expr.inclusive ? IR_CMP_ULE : IR_CMP_ULT,
                                       x, hv), tb, fb);
        return;
    }
    if (hv) ir_cmp_br(c, R->as.range_expr.inclusive ? TOKEN_ANGLE_BRACKET_LEFT_EQUAL : TOKEN_ANGLE_BRACKET_LEFT,
                      x, hv, false, ir_nonneg_lit(hi), true, tb, fb);
    else    ir_set_br(c->cur, tb);
}

// `x in [a, b, c]` (DECIDE-X, I.79): x evaluated once, then `x == a`, `x == b`, ... each on its own
// branch, the first that holds going to tb. Each element is a constant, so evaluating them in turn
// changes nothing; each comparison is mathematical whatever the types (I.88). On the edge into tb, x
// is one of the elements, so the analysis narrows it to their hull.
static void ir_list_member_br(LowerCtx *c, Expr *R, IrValue *x, IrBlock *tb, IrBlock *fb) {
    for (ExprList *el = R->as.array_literal_expr.elements; el; el = el->next) {
        IrBlock *next = el->next ? ir_new_block(c->f) : fb;
        IrValue *v = ir_lower_expr(c, el->expr);
        ir_cmp_br(c, TOKEN_EQUAL_EQUAL, x, v, false, ir_nonneg_lit(el->expr), true, tb, next);
        if (el->next) c->cur = next;
    }
}

static void ir_lower_cond_br(LowerCtx *c, Expr *cond, IrBlock *tb, IrBlock *fb) {
    if (cond && cond->kind==EXPR_BINARY && cond->as.binary_expr.op==TOKEN_KEYWORD_IN &&
        cond->as.binary_expr.right && cond->as.binary_expr.right->kind==EXPR_RANGE) {
        ir_lower_range_member(c, cond, tb, fb);
        return;
    }
    if (cond && cond->kind==EXPR_BINARY && cond->as.binary_expr.op==TOKEN_KEYWORD_IN &&
        cond->as.binary_expr.right && cond->as.binary_expr.right->kind==EXPR_ARRAY_LITERAL) {
        ir_list_member_br(c, cond->as.binary_expr.right, ir_lower_expr(c, cond->as.binary_expr.left), tb, fb);
        return;
    }
    // A comparison of a signed value with a 64-bit unsigned one branches on the sign and then on
    // the unsigned compare (I.86), so each edge keeps its fact rather than a bool's join.
    if (cond && cond->kind==EXPR_BINARY) {
        TokenKind op = cond->as.binary_expr.op; IrCmp probe;
        Expr *L = cond->as.binary_expr.left, *R = cond->as.binary_expr.right;
        if (ir_cmp_op(op, true, &probe) && L && R && L->type && R->type) {
            IrType *lt = ir_lower_type(c, L->type), *rt = ir_lower_type(c, R->type);
            if (lt && rt && lt->kind==IRT_INT && rt->kind==IRT_INT && lt->is_signed != rt->is_signed) {
                IrValue *x = ir_lower_expr(c, L), *y = ir_lower_expr(c, R);
                ir_cmp_br(c, op, x, y, ir_nonneg_lit(L), ir_nonneg_lit(R), true, tb, fb);
                return;
            }
        }
    }
    if (cond && cond->kind==EXPR_BINARY &&
        (cond->as.binary_expr.op==TOKEN_KEYWORD_AND || cond->as.binary_expr.op==TOKEN_KEYWORD_OR)) {
        bool is_and = cond->as.binary_expr.op==TOKEN_KEYWORD_AND;
        IrBlock *mid = ir_new_block(c->f);
        if (is_and) ir_lower_cond_br(c, cond->as.binary_expr.left, mid, fb);  // A false ⇒ short-circuit to fb
        else        ir_lower_cond_br(c, cond->as.binary_expr.left, tb, mid);  // A true  ⇒ short-circuit to tb
        c->cur = mid;
        ir_lower_cond_br(c, cond->as.binary_expr.right, tb, fb);
        return;
    }
    ir_set_br_cond(c->cur, ir_lower_truth(c, cond), tb, fb);
}

// Does `for <k> in 0..N { body }` fill every element of one local array? If so, hand the
// definite-init pass the same IR_INIT fact a comprehension states. Deliberately narrow — see
// the call site for why each condition is load-bearing.
static void ir_forloop_init_fact(LowerCtx *c, Stmt *s, Expr *lo_e, Expr *hi_e, bool inclusive) {
    if (!s || !lo_e || !hi_e) return;
    if (lo_e->kind != EXPR_LITERAL || lo_e->as.literal_expr.value != 0) return;
    Id *ctr = s->as.for_stmt.value_name;
    if (!ctr) return;
    // exactly one statement in the body
    StmtList *bl = s->as.for_stmt.body;
    if (!bl || bl->next || !bl->stmt || bl->stmt->kind != STMT_ASSIGN) return;
    Expr *tg = bl->stmt->as.assign_stmt.target;
    if (!tg || tg->kind != EXPR_INDEX) return;
    Expr *base = tg->as.index_expr.target, *idx = tg->as.index_expr.index;
    if (!base || base->kind != EXPR_IDENTIFIER) return;
    if (!idx || idx->kind != EXPR_IDENTIFIER) return;
    if (!id_bytes_equal(idx->as.identifier_expr.id, ctr)) return;   // indexed BY the counter
    // the array must be a fixed-length local whose length the range exactly covers
    IrLocal *loc = ir_env_find(c, base->as.identifier_expr.id);
    if (!loc || !loc->slot) return;
    IrValue *agg = loc->slot;
    // The LENGTH comes from the AST type, not from the slot's: an array alloca is typed `*T`
    // (a pointer to the ELEMENT), so the element count is nowhere on the value — it lives in
    // the alloca instruction's aux, which is not reachable from here.
    IrType *at = base->type ? ir_lower_type(c, base->type) : NULL;
    if (!at || at->kind != IRT_ARRAY || at->array_len <= 0) return;
    if (hi_e->kind == EXPR_LITERAL) {
        int64_t top = (int64_t)hi_e->as.literal_expr.value + (inclusive ? 1 : 0);
        if (top != at->array_len) return;
    } else if (hi_e->kind == EXPR_MEMBER) {
        // `0..a.len` on the SAME array is the length by definition.
        Expr *ob = hi_e->as.member_expr.target;
        Id   *mn = hi_e->as.member_expr.member;
        if (inclusive) return;
        if (!ob || ob->kind != EXPR_IDENTIFIER || !mn) return;
        if (!id_bytes_equal(ob->as.identifier_expr.id, base->as.identifier_expr.id)) return;
        if (mn->length != 3 || memcmp(mn->name, "len", 3) != 0) return;
    } else return;
    ir_init_fact(c->f, c->cur, agg);
}

static void ir_lower_stmt(LowerCtx *c, Stmt *s);
// Where `continue` goes: a `for` loop's latch (made on first use), a `while` loop's header.
static IrBlock *ir_loop_continue_target(LowerCtx *c) {
    if (!c->loop_step_cell) return c->loop_head;
    if (!c->loop_latch) c->loop_latch = ir_new_block(c->f);
    return c->loop_latch;
}
// The end of a `for` body: step the counter and go back to the header — inline when nothing
// continued (the shape every loop had), else in the latch every `continue` reached.
static void ir_lower_for_step(LowerCtx *c, IrBlock *head) {
    if (c->loop_latch) {
        if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, c->loop_latch);
        c->cur = c->loop_latch;
    } else if (ir_is_set_term(c->cur)) return;
    IrValue *ci = ir_load(c->f, c->cur, c->loop_step_cell, c->loop_step_ty);
    ir_store(c->f, c->cur, c->loop_step_cell,
             ir_binop(c->f, c->cur, IR_ADD, ci, ir_const_int(c->f, c->cur, 1, c->loop_step_ty), c->loop_step_ty));
    ir_set_br(c->cur, head);
}
// Run ONE deferred statement. It is a scope of its own: a `defer` directly in it (`defer defer
// f()`) registers above the ones being replayed and runs when the statement ends, as at the end
// of any block, and a block inside it replays its own (ir_lower_stmts). Both used to be DROPPED:
// registration was simply skipped while replaying. Control cannot leave a deferred statement
// (spec 9, E138 in the front end), so a replay never meets a `return`.
static void ir_lower_deferred(LowerCtx *c, Stmt *s) {
    int mark = c->ndefers; bool o = c->in_defer;
    c->in_defer = true;
    ir_lower_stmt(c, s);
    for (int i = c->ndefers - 1; i >= mark; i--) ir_lower_deferred(c, c->defers[i]);
    c->ndefers = mark;
    c->in_defer = o;
}
// Replay the deferred statements registered at or above `from`, newest first, without popping
// them: an early exit runs the defers registered SO FAR, and a later exit runs them too. One
// dynamic execution reaches exactly one exit, so replaying at each is faithful, and it is what
// makes `defer drop(mov r); drop(mov r)` visible as the double consume it is.
static void ir_lower_replay_defers(LowerCtx *c, int from) {
    for (int i = c->ndefers - 1; i >= from; i--) ir_lower_deferred(c, c->defers[i]);
}
// A `return` (or a `try` that propagates) leaves every scope at once.
static void ir_lower_flush_defers(LowerCtx *c) {
    // Inside a replay this would run the statement being replayed again. The front end refuses
    // it (E138); refuse here too rather than skip the rest of the replay, which this used to do.
    if (c->in_defer) { ir_incomplete(c, "control leaving a deferred statement"); return; }
    ir_lower_replay_defers(c, 0);
}

// ★ A STORE TO A FIELD MUST KEEP THE STRUCT'S INVARIANT. `pos usize in 0..text.len` is read as a
// FACT (every load of `pos` assumes `pos < text.len`, which is what proves `c.text[c.pos]`), so every
// write that could break it must pay for it. Construction asserted it; ASSIGNMENT did not — the
// legacy front-end E121 covered fixed-array containers only and accepted a slice container
// unconditionally — so `c.pos = 5` over a 4-byte slice compiled and the proven read overflowed
// the buffer (ASan). Two ways to break it, both asserted below by the relational rules:
//   · storing the constrained field itself: the new value must satisfy the relation;
//   · storing the field it names (the container): every field related to it must still hold.
static IrInstr *ir_def_in_block(IrBlock *b, IrValue *v) {
    for (IrInstr *i = b ? b->instrs : NULL; i; i = i->next) if (i->result == v) return i;
    return NULL;
}
// `fp` is the target's defining instruction, captured BEFORE the right-hand side was lowered: an
// `and`/`or` or a conditional there opens new blocks, and a lookup afterwards would miss it and
// silently emit no assert — fail-open.
static void ir_lower_field_invariant_asserts(LowerCtx *c, IrInstr *fp, IrValue *v) {
    if (!fp || fp->op != IR_FIELD_PTR || fp->n_operands < 1 || !v || !v->type) return;
    IrValue *base = fp->operands[0];
    IrType *bt = base && base->type ? base->type : NULL;
    IrType *sty = ir_struct_of(bt && bt->kind == IRT_PTR ? bt->elem : bt);
    if (!sty || sty->kind != IRT_STRUCT) return;
    int fi = fp->aux.field_idx;
    if (fi < 0 || fi >= sty->n_fields) return;
    // The FIELD's declared type, never the assigned value's: `b.pos = 3` assigns an i32 literal
    // to a usize field, and a field pointer typed from the value was `int32_t *` over a
    // `uint64_t` field — C that gcc rejects.
    IrType *ft = sty->fields[fi];
    ir_field_len_bound_fact(c, sty, fi, v, true);                // a new slice keeps its length bound
    // RELATIONAL invariants. The field written is either the constrained one (its new value must
    // satisfy the relation against the other's CURRENT value) or the one a relation names (every
    // field constrained against it must satisfy the relation against the NEW value). In both, the
    // relation held before the store — every write that could break it is asserted — so it is
    // assumed on the old values first: that is what proves `b.len = b.len + 1` under
    // `if b.len < b.cap`, and `l.pos = l.pos + 1` under `if l.pos < l.src.len`.
    { IrFieldRel rels[8];
      int nr = (ft && ft->kind == IRT_INT) ? ir_field_relations(c, sty, fi, rels, 8) : 0;
      for (int q = 0; q < nr; q++) {
          IrValue *rv = ir_field_rel_rhs_at(c, base, sty, &rels[q], ft);
          if (!rv) continue;
          IrValue *old = ir_load(c->f, c->cur, ir_field_ptr(c->f, c->cur, base, fi, ft), ft);
          ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, rels[q].cmp, old, rv));
          if (v->type->kind == IRT_INT)
              ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, rels[q].cmp, v, rv), 121);
      } }
    for (int j = 0; j < sty->n_fields; j++) {
        if (j == fi) continue;
        IrFieldRel rels[8];
        IrType *jt = sty->fields[j];
        int nr = (jt && jt->kind == IRT_INT) ? ir_field_relations(c, sty, j, rels, 8) : 0;
        for (int q = 0; q < nr; q++) {
            if (rels[q].other != fi) continue;
            IrValue *jv = ir_load(c->f, c->cur, ir_field_ptr(c->f, c->cur, base, j, jt), jt);
            IrValue *orv = ir_field_rel_rhs_at(c, base, sty, &rels[q], jt);
            if (orv) ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, rels[q].cmp, jv, orv));
            IrValue *nrv = ir_field_rel_rhs(c, v, sty->fields[fi], rels[q].is_len, jt);
            if (nrv) ir_assert_coded(c->f, c->cur, ir_icmp(c->f, c->cur, rels[q].cmp, jv, nrv), 121);
        }
    }
}

static void ir_lower_stmt_body(LowerCtx *c, Stmt *s);
static void ir_lower_stmt(LowerCtx *c, Stmt *s) {
    if (s && s->kind == STMT_VAR && !s->as.var_stmt.is_mutable && c->f && !ir_is_set_term(c->cur)) {
        IrSealMark m = ir_seal_open(c);
        ir_lower_stmt_body(c, s);
        ir_seal_close(c, m);
        return;
    }
    ir_lower_stmt_body(c, s);
}
static void ir_lower_stmt_body(LowerCtx *c, Stmt *s) {
    if (s && s->line) { ir_cur_line = s->line; ir_cur_col = s->col; }
    if (!s || ir_is_set_term(c->cur)) return;   // dead code after a terminator
    switch (s->kind) {
        case STMT_VAR: {
            IrType *slot_ty = s->as.var_stmt.type ? ir_lower_borrow_binding_type(c, s->as.var_stmt.type)
                            : (s->as.var_stmt.expr ? ir_lower_borrow_binding_type(c, s->as.var_stmt.expr->type)
                                                   : ir_type_int(c->a,32,true));
            if (ir_type_is_agg(slot_ty)) {
                IrValue *agg = slot_ty->kind==IRT_ARRAY ? ir_alloca_array(c->f, c->cur, slot_ty)
                                                        : ir_alloca(c->f, c->cur, slot_ty);
                IrLocal *l = arena_push_aligned(c->a, IrLocal);
                l->name = s->as.var_stmt.name; l->slot = agg; l->param = NULL;
                if (l->name && !agg->src_name) agg->src_name = ir_intern(c->a, l->name->name, l->name->length);
                l->aggregate = true; l->next = c->locals; c->locals = l;
                // array-literal initializer: store each element at its index
                Expr *init = s->as.var_stmt.expr;
                if (init && init->kind == EXPR_ARRAY_LITERAL && slot_ty->kind==IRT_ARRAY) {
                    int k = 0;
                    for (ExprList *el = init->as.array_literal_expr.elements; el; el = el->next, k++) {
                        IrValue *idx = ir_const_int(c->f, c->cur, k, ir_type_int(c->a,64,false));
                        IrValue *p   = ir_elem_ptr(c->f, c->cur, agg, idx, slot_ty->elem);
                        ir_store(c->f, c->cur, p, ir_lower_expr(c, el->expr));
                    }
                } else if (init && init->kind == EXPR_ARRAY_COMPREHENSION && slot_ty->kind==IRT_ARRAY) {
                    // [ body for i in range ] ⇒ i=lo; while i<hi { agg[i]=body; i+=1 }
                    Expr *rng=init->as.array_comprehension_expr.range, *bod=init->as.array_comprehension_expr.body;
                    IrType *ity=ir_type_int(c->a,64,false);
                    bool isr = rng && rng->kind==EXPR_RANGE;
                    IrValue *icell=ir_alloca(c->f,c->cur,ity);
                    ir_store(c->f,c->cur,icell, (isr&&rng->as.range_expr.start)?ir_lower_expr(c,rng->as.range_expr.start):ir_const_int(c->f,c->cur,0,ity));
                    ir_env_add(c, init->as.array_comprehension_expr.idx, icell, NULL);
                    IrBlock *head=ir_new_block(c->f),*bb=ir_new_block(c->f),*ex=ir_new_block(c->f);
                    ir_set_br(c->cur,head); c->cur=head;
                    IrValue *iv=ir_load(c->f,head,icell,ity);
                    IrValue *hi=(isr&&rng->as.range_expr.end)?ir_lower_expr(c,rng->as.range_expr.end):ir_const_int(c->f,head,slot_ty->array_len,ity);
                    ir_set_br_cond(head, ir_icmp(c->f,head,(isr&&rng->as.range_expr.inclusive)?IR_CMP_ULE:IR_CMP_ULT,iv,hi), bb, ex);
                    c->cur=bb;
                    // The element INDEX is the POSITION, not the loop variable: `[x + 1 for x
                    // in 1..=3]` runs x over 1..3 and fills positions 0..2. Storing at `x`
                    // left inc[0] unwritten and wrote one past the end — reading garbage from
                    // an array the comprehension claims to fill completely.
                    IrValue *iv2=ir_load(c->f,bb,icell,ity);
                    IrValue *pos=iv2;
                    if (isr && rng->as.range_expr.start) {
                        IrValue *lo0 = ir_lower_expr(c, rng->as.range_expr.start);
                        pos = ir_binop(c->f,bb,IR_SUB,iv2,lo0,ity);
                    }
                    ir_store(c->f,bb, ir_elem_ptr(c->f,bb,agg,pos,slot_ty->elem), ir_lower_expr(c,bod));
                    IrValue *ci=ir_load(c->f,c->cur,icell,ity);
                    ir_store(c->f,c->cur,icell, ir_binop(c->f,c->cur,IR_ADD,ci,ir_const_int(c->f,c->cur,1,ity),ity));
                    ir_set_br(c->cur,head); c->cur=ex;
                    // A comprehension fills EVERY element by definition. State it: the fill
                    // LOOP's zero-iteration path would otherwise intersect the fact away.
                    ir_init_fact(c->f, c->cur, agg);
                } else if (init && slot_ty->kind==IRT_ARRAY && slot_ty->array_len > 0
                           && ir_lower_type(c, init->type)
                           && ir_lower_type(c, init->type)->kind==IRT_ARRAY) {
                    // WHOLE-ARRAY COPY. `var a i32[4] = b` has VALUE semantics — C forbids
                    // array `=`, so it must be a copy, and it was falling to the unmodelled
                    // opaque instead: `a` held garbage and mutating `b` afterwards was
                    // indistinguishable from not copying at all. Element-wise rather than a
                    // memcpy op, because the length is a compile-time constant and every
                    // element store is a fact definite-assignment can use.
                    IrValue *src = ir_lower_expr(c, init);
                    IrType *ity2 = ir_type_int(c->a,64,false);
                    for (int64_t q = 0; q < slot_ty->array_len; q++) {
                        IrValue *idx = ir_const_int(c->f, c->cur, q, ity2);
                        IrValue *sp  = ir_elem_ptr(c->f, c->cur, src, idx, slot_ty->elem);
                        IrValue *dp  = ir_elem_ptr(c->f, c->cur, agg, idx, slot_ty->elem);
                        ir_store(c->f, c->cur, dp, ir_load(c->f, c->cur, sp, slot_ty->elem));
                    }
                    ir_init_fact(c->f, c->cur, agg);
                } else if (init && init->kind == EXPR_STRING && slot_ty->kind==IRT_ARRAY
                           && slot_ty->elem && slot_ty->elem->kind==IRT_INT
                           && slot_ty->elem->bits==8) {
                    // `src u8[5] = "hello"` — a byte array initialised from a string LITERAL.
                    // It fell to the opaque, so the array held garbage and the program printed
                    // nothing where it should print `h`. The bytes are known at compile time,
                    // so write them: one store per element makes every element DEFINITELY
                    // INITIALISED, which the opaque could only approximate.
                    int32_t n = init->as.string_expr.length;
                    if (n > slot_ty->array_len) n = (int32_t)slot_ty->array_len;
                    for (int32_t q = 0; q < n; q++) {
                        IrValue *idx = ir_const_int(c->f, c->cur, q, ir_type_int(c->a,64,false));
                        IrValue *p   = ir_elem_ptr(c->f, c->cur, agg, idx, slot_ty->elem);
                        ir_store(c->f, c->cur, p,
                                 ir_const_int(c->f, c->cur,
                                              (unsigned char)init->as.string_expr.value[q],
                                              slot_ty->elem));
                    }
                    // A shorter literal leaves the tail zero, exactly as C does.
                    for (int64_t q = n; q < slot_ty->array_len; q++) {
                        IrValue *idx = ir_const_int(c->f, c->cur, q, ir_type_int(c->a,64,false));
                        IrValue *p   = ir_elem_ptr(c->f, c->cur, agg, idx, slot_ty->elem);
                        ir_store(c->f, c->cur, p, ir_const_int(c->f, c->cur, 0, slot_ty->elem));
                    }
                    ir_init_fact(c->f, c->cur, agg);
                } else if (init) {
                    // An initialiser shape we do not model. The aggregate IS initialised —
                    // that is what an initialiser means — but with content we cannot describe.
                    // Say exactly that: an opaque write over it, then the init fact. Marking
                    // the whole function incomplete threw away every unrelated proof too.
                    { IrValue *rd[1]; rd[0] = agg;
                      ir_opaque(c->f, c->cur, NULL, true, "aggregate-init", rd, 1);
                      ir_init_fact(c->f, c->cur, agg); }
                }
                break;
            }
            IrValue *slot = ir_alloca(c->f, c->cur, slot_ty);
            // A VECTOR local initialised from a bracket literal. Not the array path — a vector
            // is a VALUE, so it is built once and stored, not filled lane by lane. The literal
            // itself types as an array; the vector comes from the DECLARED slot type, which is
            // why this cannot be decided from the initializer alone.
            if (slot_ty->kind == IRT_VECTOR && s->as.var_stmt.expr
                && s->as.var_stmt.expr->kind == EXPR_ARRAY_LITERAL) {
                Expr *vi = s->as.var_stmt.expr;
                int n = 0; for (ExprList *el = vi->as.array_literal_expr.elements; el; el = el->next) n++;
                IrValue **lanes = arena_push_many_aligned(c->a, IrValue*, n > 0 ? n : 1);
                int k = 0;
                for (ExprList *el = vi->as.array_literal_expr.elements; el; el = el->next, k++)
                    lanes[k] = ir_lower_expr(c, el->expr);
                ir_store(c->f, c->cur, slot, ir_struct_new(c->f, c->cur, slot_ty, lanes, n));
                IrLocal *vl = arena_push_aligned(c->a, IrLocal);
                vl->name = s->as.var_stmt.name; vl->slot = slot; vl->param = NULL;
                vl->next = c->locals; c->locals = vl;
                break;
            }
            // A stack VLA — `var a u8[n]`. It lowered to a SLICE SLOT WITH NO STORAGE and an
            // ASSUME that its length is n: the length was then known to the prover while the
            // data pointer stayed null, so `a[i] = 1` wrote through null and the program
            // SEGFAULTED on a program the analysis had just proven safe. An assumed fact with
            // no allocation behind it is exactly the shape this project treats as a hole.
            //
            // Allocate it. IR_ALLOCA with a COUNT operand is a dynamic frame allocation — a C
            // VLA, LLVM's `alloca T, n`, a Zig stack buffer — and the slice built over it
            // carries the length as a VALUE, so nothing has to be assumed at all.
            {   Type *dt = s->as.var_stmt.type;
                if (dt && dt->kind==TYPE_ARRAY && dt->array_len<0 && dt->size_expr
                    && slot_ty->kind==IRT_SLICE && !s->as.var_stmt.expr) {
                    IrValue *nv = ir_lower_expr(c, dt->size_expr);
                    if (nv && nv->type && nv->type->kind==IRT_INT) {
                        IrType *el = slot_ty->elem ? slot_ty->elem : ir_type_int(c->a,8,false);
                        IrValue *base = ir_alloca_dyn(c->f, c->cur, el, nv);
                        ir_store(c->f, c->cur, slot, ir_make_slice(c->f, c->cur, base, nv, el));
                    }
                }
            }
            Expr *vinit = s->as.var_stmt.expr;
            // `var s = "hello"` — a MUTABLE binding initialised from a string literal needs
            // its OWN writable storage. IR_STR_CONST points at IMMUTABLE STATIC bytes (that
            // is the primitive's meaning, and the emitter honours it as a C string literal),
            // so binding a writable slice straight to it is a lie: the first `s[0] = c`
            // faulted. Materialise the copy the source semantics ask for.
            //
            // Two things fall out. The slice now roots in an ALLOCA, so escape analysis sees
            // the truth when one is returned; and an IMMUTABLE binding still points at the
            // static bytes, costing nothing — which is the right default under a
            // performance-first design law.
            if (vinit && vinit->kind == EXPR_STRING && s->as.var_stmt.is_mutable
                && slot_ty->kind == IRT_SLICE) {
                int32_t n = (int32_t)vinit->as.string_expr.length;
                const char *bytes = vinit->as.string_expr.value;
                IrType *u8t = ir_type_int(c->a, 8, false);
                IrType *arr = ir_type_new(c->a, IRT_ARRAY); arr->elem = u8t; arr->array_len = n+1;
                IrValue *buf = ir_alloca_array(c->f, c->cur, arr);
                IrType *idxt = ir_type_int(c->a, 64, false);
                for (int32_t k = 0; k <= n; k++) {         // includes the NUL sentinel
                    IrValue *ix = ir_const_int(c->f, c->cur, k, idxt);
                    ir_store(c->f, c->cur, ir_elem_ptr(c->f, c->cur, buf, ix, u8t),
                             ir_const_int(c->f, c->cur, k<n ? (unsigned char)bytes[k] : 0, u8t));
                }
                IrValue *ln = ir_const_int(c->f, c->cur, n, idxt);
                ir_store(c->f, c->cur, slot, ir_make_slice(c->f, c->cur, buf, ln, u8t));
            } else if (vinit) {
                // `s u8[] = buf` — the DECLARED type's representation (see ir_coerce_repr). The
                // array's base pointer was stored into the slice slot: C that gcc rejects.
                ir_store(c->f, c->cur, slot, ir_coerce_repr(c, ir_lower_expr(c, vinit), slot_ty, vinit));
            }
            ir_env_add(c, s->as.var_stmt.name, slot, NULL);
            break;
        }
        case STMT_ASSIGN: {   // (an `unsafe` store carries the same waiver — see below)
            // WHOLE-ARRAY ASSIGNMENT has VALUE semantics — `c = a` copies, and C forbids array
            // `=`, so a single store would emit either broken C or a pointer aliasing the
            // source. Same element-wise copy as the copy-initialiser, for the same reason:
            // the length is a compile-time constant and every store is a fact.
            { Type *tt = s->as.assign_stmt.target ? s->as.assign_stmt.target->type : NULL;
              IrType *at = tt ? ir_lower_type(c, tt) : NULL;
              // sema does not always type an assignment's LHS, and the RHS is the same array
              // type by construction — so ask it when the target has nothing to say. ONLY then:
              // a target typed as a SLICE (`w.s = buf`) is not an array copy but a decay, and
              // asking the RHS here turned it into an element-wise copy into a slice of
              // unknown length — refused as E085 on a correct program.
              if (!at) {
                  Type *st2 = s->as.assign_stmt.expr ? s->as.assign_stmt.expr->type : NULL;
                  IrType *st3 = st2 ? ir_lower_type(c, st2) : NULL;
                  if (st3 && st3->kind == IRT_ARRAY) at = st3;
              }
              if (at && at->kind==IRT_ARRAY && at->array_len > 0) {
                  IrValue *dst = ir_lower_expr(c, s->as.assign_stmt.target);
                  IrValue *src = ir_lower_expr(c, s->as.assign_stmt.expr);
                  IrType *ity3 = ir_type_int(c->a,64,false);
                  // NOT keyed on the values' IR types: an array LOCAL decays to a base
                  // pointer typed `*T`, while an array FIELD carries `[N]T` — the model is
                  // uniform in behaviour, not in spelling. The DECLARED type is what says
                  // this is an array assignment.
                  if (dst && src) {
                      for (int64_t q=0; q<at->array_len; q++) {
                          IrValue *idx = ir_const_int(c->f, c->cur, q, ity3);
                          IrValue *sp  = ir_elem_ptr(c->f, c->cur, src, idx, at->elem);
                          IrValue *dp  = ir_elem_ptr(c->f, c->cur, dst, idx, at->elem);
                          ir_store(c->f, c->cur, dp, ir_load(c->f, c->cur, sp, at->elem));
                      }
                      break;
                  }
              } }
            IrValue *addr = ir_lower_addr(c, s->as.assign_stmt.target);
            // Writing through a reference binding (`var r = f(var c); r = 42`) stores into the
            // referent, not over the pointer in r's slot: ir_lower_addr already resolves r to
            // the address it holds (ir_ref_binding_ptr). Without that the owner never saw the
            // write — D-38, which the AST emitter fixed first and the IR then reproduced.
            { IrInstr *tdef = (addr && c->cur) ? ir_def_in_block(c->cur, addr) : NULL;
              IrValue *rv0 = ir_lower_expr(c, s->as.assign_stmt.expr);
              // The target's representation, read from the address: sema does not always type
              // an assignment's LHS, and the slot or field the address names always has one.
              // `w.s = buf` stored the array's base pointer into a slice.
              if (addr && addr->type && addr->type->kind==IRT_PTR && addr->type->elem)
                  rv0 = ir_coerce_repr(c, rv0, addr->type->elem, s->as.assign_stmt.expr);
              // ★ NOT WAIVED BY `unsafe`. Every read of the field ASSUMES the invariant (that is
              // what proves `b.cap - b.len` and `text[pos]`), and the C hands each assume to gcc
              // as `__builtin_unreachable`. Skipping the assert inside `unsafe` let
              // `unsafe { b.len = 4 }` over `cap = 2` compile, and a later `b.cap - b.len`
              // segfaulted at -O2 while -O0 printed the underflow (Handwriting, M15). Spec 18
              // waives bounds, dereference, casts and field access in `unsafe`; an invariant is
              // part of the struct's type, which `unsafe` does not suspend.
              ir_lower_field_invariant_asserts(c, tdef, rv0);
              ir_store(c->f, c->cur, addr, rv0); }
            if (c->cur->instrs_tail) c->cur->instrs_tail->unchecked = c->unsafe;
            break;
        }
        case STMT_EXPR: {
            Expr *x = s->as.expr_stmt.expr;
            (void)ir_lower_expr(c, x);
            // ★ A `panic(...)` STATEMENT ENDS ITS PATH, and so does a call to a [noreturn]
            // function. `else panic(...)` set the block unreachable, but the statement form fell
            // through like any call, so after
            //     if s.len == 0 { panic("empty") }
            // the analysis still had s.len == 0 at `s[0]` and refused it (E085), while the spec
            // says panic never returns (its helper calls abort()).
            //
            // A [noreturn] callee was excluded as "an unchecked claim". Since 9cbe203 a body's
            // [noreturn] IS checked (E135: every return of it must be unreachable), so trusting
            // it is sound; partial correctness, and a cycle of [noreturn] functions never
            // returns either. An `extern [noreturn]` is BELIEVED, exactly as its effect row is,
            // and gcc already treats the code after it as unreachable. Without this, a
            // [noreturn] function could not end in `exit(1)` or in another [noreturn] function,
            // which E135's own message offers ("a call that does not return").
            if (x && x->kind == EXPR_CALL && x->as.call_expr.callee &&
                x->as.call_expr.callee->kind == EXPR_IDENTIFIER && !ir_is_set_term(c->cur)) {
                Decl *cd = x->as.call_expr.callee->decl;
                Id *nm = x->as.call_expr.callee->as.identifier_expr.id;
                if (!cd && nm && nm->length == 5 && strncmp(nm->name, "panic", 5) == 0)
                    ir_set_unreachable(c->cur);
                else if (cd && (cd->kind == DECL_FUNCTION || cd->kind == DECL_EXTERN_FUNCTION) &&
                         cd->as.function_decl.is_noreturn)
                    ir_set_unreachable(c->cur);
            }
            break;
        }
        case STMT_DEFER:
            // Recorded, not emitted: the body runs at every exit of this block, newest first.
            // Inside a deferred statement too, which is a scope like any other.
            if (c->ndefers == c->defers_cap) {
                int nc = c->defers_cap ? 2 * c->defers_cap : 16;
                Stmt **nd = arena_push_many_aligned(c->a, Stmt *, nc);
                if (c->ndefers) memcpy(nd, c->defers, (size_t)c->ndefers * sizeof *nd);
                c->defers = nd; c->defers_cap = nc;
            }
            c->defers[c->ndefers++] = s->as.defer_stmt.stmt;
            break;
        case STMT_RETURN: {
            // ORDER MATTERS, and it is not Go's. Lain runs the deferred statements BEFORE
            // evaluating the return expression, so a defer CAN change what is returned:
            //     var acc = 0; defer acc = acc+1; defer acc = acc*10; acc = 5; return acc
            // yields 51, not 5. (Go copies the return value first and would give 5.) Verified
            // against the old backend, which emits the defers then `return acc;` — computing
            // the value first made the new pipeline return 5, a silent miscompile.
            ir_lower_flush_defers(c);
            // A deferred `panic` or [noreturn] call ends the path here: the return never happens,
            // and setting it would overwrite the block's `unreachable` with a reachable `ret`.
            if (ir_is_set_term(c->cur)) break;
            IrValue *rv = s->as.return_stmt.value ? ir_lower_expr(c, s->as.return_stmt.value) : NULL;
            // `return buf` from a function returning `u8[]`: the declared return type's
            // representation (see ir_coerce_repr) — a bare base pointer was returned as a slice.
            if (rv) rv = ir_coerce_repr(c, rv, c->f->ret_type, s->as.return_stmt.value);
            if (rv) ir_lower_return_ensures_assert(c, rv);   // callee proves its own ensures
            ir_set_ret(c->cur, rv);
            break;
        }
        case STMT_IF: {
            IrBlock *tb = ir_new_block(c->f), *jb = ir_new_block(c->f);
            IrBlock *eb = s->as.if_stmt.else_branch ? ir_new_block(c->f) : jb;
            ir_lower_cond_br(c, s->as.if_stmt.cond, tb, eb);   // expands `and`/`or` (refinement-preserving)
            c->cur = tb;
            // NARROWING. `if x { use_it(x) }` over an optional sum makes `x` the PAYLOAD
            // inside the branch — the language's whole point in replacing `!= nil` guards.
            // The lowering passed the sum straight through, so the callee received a
            // two-variant struct where a pointer was declared. Rebinding the name for the
            // duration of the then-branch is what the front end's flow-narrowing means, said
            // in the IR: the env is a prepend-only list, so restoring the head unbinds it.
            IrLocal *saved = c->locals;
            { Expr *cx = s->as.if_stmt.cond;
              if (cx && cx->kind==EXPR_IDENTIFIER) {
                  IrLocal *lo = ir_env_find(c, cx->as.identifier_expr.id);
                  IrValue *cv = lo ? (lo->param ? lo->param : lo->slot) : NULL;
                  int sk, nk;
                  if (cv && cv->type && ir_sum_optional_shape(cv->type, &sk, &nk))
                      ir_env_add(c, cx->as.identifier_expr.id, NULL,
                                 ir_sum_optional_payload(c, cv, sk));
              } }
            ir_lower_stmts(c, s->as.if_stmt.then_body);
            c->locals = saved;
            if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, jb);
            if (s->as.if_stmt.else_branch) {
                c->cur = eb; ir_lower_stmts(c, s->as.if_stmt.else_branch);
                if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, jb);
            }
            c->cur = jb;
            break;
        }
        case STMT_WHILE: {
            IrBlock *head = ir_new_block(c->f), *body = ir_new_block(c->f), *exit = ir_new_block(c->f);
            ir_set_br(c->cur, head);
            c->cur = head;
            // ★ THE CONDITION IS LOWERED AS CONTROL FLOW, NOT AS A VALUE — the same helper
            // `if` uses, and for the same reason. Materialising `A and B` as a bool builds a
            // temp slot written on two paths and a JOIN before the branch, so the fact from A
            // (established on the edge into B's block) is hulled against the path where A is
            // false. Measured: `while i < 100 { a[i] }` proves check-free and
            // `while i < 100 and s < 5 { a[i] }` does not — ANY `and` in a while guard lost
            // BOTH conjuncts, while the identical `and` in an `if` proved, because `if` was
            // already going through here. Short-circuiting through blocks puts each conjunct's
            // fact on its own edge, where a guard refinement can see it.
            //
            // The helper sets the terminator on the block the condition FINISHES in, which is
            // also what the previous code had to do by hand: a short-circuit condition leaves
            // `c->cur` past the header, and setting the terminator on `head` overwrote the edge
            // INTO those blocks — the loop then branched on a value nothing had computed, an
            // infinite loop, silently. `head` remains the loop header: the back edge from the
            // body still targets it, which is what the natural-loop detection keys on.
            // D-44: carry the SOURCE's `decreasing` clause onto the header. A written measure
            // is a claim the compiler defends wherever it appears — including in a `proc`,
            // where totality is not required and the sovereign engine would otherwise raise no
            // obligation at all. Without this the fact reached no analysis and three corpus
            // programs asserting a bad measure compiled.
            head->has_measure = s->as.while_stmt.measure_written;
            // D-49: record the guard's values so the measure can reuse them (see LowerCtx).
            int cse_save = c->cse_n; bool cse_rec_save = c->cse_recording;
            c->cse_n = 0; c->cse_recording = (s->as.while_stmt.measure_written != false);
            ir_lower_cond_br(c, s->as.while_stmt.cond, body, exit);
            c->cse_recording = false;
            IrBlock *oh=c->loop_head, *oe=c->loop_exit; int om=c->loop_defer_mark;
            IrValue *osc=c->loop_step_cell; IrType *ost=c->loop_step_ty; IrBlock *olt=c->loop_latch;
                c->loop_head=head; c->loop_exit=exit; c->loop_defer_mark=c->ndefers;
                c->loop_step_cell=NULL; c->loop_step_ty=NULL; c->loop_latch=NULL;   // continue: the header
            c->cur = body;
            // ── D-49: THE MEASURE IS AN EXPRESSION, AND IT HAS TO BE WELL-DEFINED ────────
            // A written `decreasing` is a claim the compiler defends (D-44), and a claim that
            // is not well-defined defends nothing: `while i > 0 decreasing n - i` UNDERFLOWS at
            // n == 0, and `n - i` is SIZE_MAX rather than a measure. The IR recorded only THAT
            // a measure existed, never WHAT it was, so the sovereign engine had nothing to say
            // and the legacy overflow path was the only thing catching it.
            //
            // Lowering it puts the expression under the ordinary arithmetic obligations with no
            // special case. Two things about WHERE and HOW, both measured rather than guessed:
            //
            //   IN THE BODY, UNDER THE GUARD. At the header the measure had to be well-defined
            //   on the EXIT path too, where `i < n` does not hold — 8 over-rejections, since
            //   `decreasing n - i` is the commonest measure in the corpus. A measure's job is
            //   to fall on each ITERATION, so where it must be defined is where one happens.
            //
            //   REUSING THE GUARD'S VALUES. Lowering it fresh re-computes shared subexpressions
            //   (`while i < n / 2 decreasing n / 2 - i` builds a SECOND `n / 2`), and a
            //   relational domain relates VALUES, not syntax — so the octagon could not connect
            //   the measure's copy to the one the guard bounds `i` against. The cse memo above
            //   makes both spellings denote one value.
            //
            // The value is discarded; only its obligations matter. Nothing is stored, so no
            // analysis sees a new write and the loop's shape is unchanged.
            if (s->as.while_stmt.measure_written && s->as.while_stmt.measure)
                head->measure_val = ir_lower_expr(c, s->as.while_stmt.measure);   // I.74: kept
            c->cse_n = cse_save; c->cse_recording = cse_rec_save;
            ir_lower_stmts(c, s->as.while_stmt.body);
            if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, head);
            c->loop_head=oh; c->loop_exit=oe; c->loop_defer_mark=om;
            c->loop_step_cell=osc; c->loop_step_ty=ost; c->loop_latch=olt;
            c->cur = exit;
            break;
        }
        case STMT_FOR: {
            Expr *it = s->as.for_stmt.iterable;
            Id *vn = s->as.for_stmt.value_name, *xn = s->as.for_stmt.index_name;
            IrType *usz = ir_type_int(c->a, 64, false);
            if (it && it->kind == EXPR_RANGE) {
                // for i in lo..hi { body }  ⇒  i=lo; while i <(=) hi { body; i=i+1 }
                Expr *lo_e=it->as.range_expr.start, *hi_e=it->as.range_expr.end;
                // ★ The counter takes the type of the BOUND, not of the start. `for i in
                // 0..out.len` was typing `i` from the literal `0`, so an i32 counter got
                // compared against a usize length; nothing bounds a length below INT32_MAX,
                // so `i + 1` can genuinely leave i32 and the overflow check refuses it —
                // correctly, on the most ordinary loop in the language. The A1 survey
                // measured this shape at 12% of every unproven obligation in the corpus.
                // The bound already carries the right type; the counter just was not asking.
                IrType *ity = hi_e ? ir_lower_type(c, hi_e->type) : NULL;
                if (!ity || ity->kind!=IRT_INT)
                    ity = lo_e ? ir_lower_type(c, lo_e->type) : usz;
                if (!ity || ity->kind!=IRT_INT) ity = usz;
                IrValue *icell = ir_alloca(c->f, c->cur, ity);
                ir_store(c->f, c->cur, icell, lo_e ? ir_lower_expr(c, lo_e) : ir_const_int(c->f,c->cur,0,ity));
                ir_env_add(c, vn, icell, NULL);                 // i reads/writes its cell
                // `for i, val in 0..5` binds BOTH names to the counter — over a range the
                // index and the value are the same quantity, which is what the old backend
                // emits (`size_t i = __i0; int val = (int)__i0;`). The second name was simply
                // never bound, so it read whatever `i` meant outside the loop and printed 0
                // every iteration.
                if (xn) ir_env_add(c, xn, icell, NULL);
                IrBlock *head=ir_new_block(c->f), *body=ir_new_block(c->f), *exit=ir_new_block(c->f);
                ir_set_br(c->cur, head); c->cur = head;
                IrValue *iv = ir_load(c->f, head, icell, ity);
                IrValue *hi = hi_e ? ir_lower_expr(c, hi_e) : ir_const_int(c->f,head,0,ity);
                IrValue *cond = ir_icmp(c->f, head, it->as.range_expr.inclusive?IR_CMP_ULE:IR_CMP_ULT, iv, hi);
                ir_set_br_cond(head, cond, body, exit);
                IrBlock *oh=c->loop_head, *oe=c->loop_exit; int om=c->loop_defer_mark;
                IrValue *osc=c->loop_step_cell; IrType *ost=c->loop_step_ty; IrBlock *olt=c->loop_latch;
                c->loop_head=head; c->loop_exit=exit; c->loop_defer_mark=c->ndefers;
                c->loop_step_cell=icell; c->loop_step_ty=ity; c->loop_latch=NULL;
                c->cur = body; ir_lower_stmts(c, s->as.for_stmt.body);
                ir_lower_for_step(c, head);
                c->loop_head=oh; c->loop_exit=oe; c->loop_defer_mark=om;
                c->loop_step_cell=osc; c->loop_step_ty=ost; c->loop_latch=olt; c->cur = exit;
                // ★ A LOOP THAT FILLS EVERY ELEMENT IS A WHOLE-INITIALISATION.
                // `for k in 0..8 { a[k] = e }` over `a i32[8]` writes every slot, but
                // definite-init is a MUST analysis over a per-element lattice and the store is
                // at a symbolic index, so it learned nothing and `a[7]` afterwards was [E005].
                // The comprehension form already states the fact with IR_INIT; a fill loop is
                // the same fact written differently, and fuzz_init.sh measured it as the ONLY
                // over-strict shape it could produce (8 of 8 in a 60-program run).
                //
                // Recognised narrowly, because the fact is unconditional: the range must start
                // at 0 and end at the array's own length, and the body must be exactly ONE
                // assignment whose target is that array indexed by the loop counter. A
                // conditional store, a second statement, a `break` or a different index all
                // fall out and the loop stays unproven — which is the safe direction.
                ir_forloop_init_fact(c, s, lo_e, hi_e, it->as.range_expr.inclusive);
                break;
            }
            // for v in arr { body }  ⇒  i=0; while i<arr.len { v=arr[i]; body; i=i+1 }
            IrValue *av = ir_lower_expr(c, it);
            IrType  *aty = it ? ir_lower_type(c, it->type) : NULL;
            IrType  *elem = aty && aty->elem ? aty->elem : ir_type_int(c->a,32,true);
            bool is_slice = av->type && av->type->kind==IRT_SLICE;
            int64_t alen = (it && it->type && it->type->kind==TYPE_ARRAY) ? it->type->array_len : -1;
            IrValue *icell = ir_alloca(c->f, c->cur, usz);
            ir_store(c->f, c->cur, icell, ir_const_int(c->f,c->cur,0,usz));
            IrValue *vcell = ir_alloca(c->f, c->cur, elem);
            ir_env_add(c, vn, vcell, NULL);
            if (xn) ir_env_add(c, xn, icell, NULL);
            IrBlock *head=ir_new_block(c->f), *body=ir_new_block(c->f), *exit=ir_new_block(c->f);
            ir_set_br(c->cur, head); c->cur = head;
            IrValue *iv = ir_load(c->f, head, icell, usz);
            IrValue *len = is_slice ? ir_slice_len(c->f, head, av)
                         : ir_const_int(c->f, head, alen>=0?alen:0, usz);
            ir_set_br_cond(head, ir_icmp(c->f,head,IR_CMP_ULT,iv,len), body, exit);
            IrBlock *oh=c->loop_head, *oe=c->loop_exit; int om=c->loop_defer_mark;
            IrValue *osc=c->loop_step_cell; IrType *ost=c->loop_step_ty; IrBlock *olt=c->loop_latch;
                c->loop_head=head; c->loop_exit=exit; c->loop_defer_mark=c->ndefers;
                c->loop_step_cell=icell; c->loop_step_ty=usz; c->loop_latch=NULL;
            c->cur = body;
            IrValue *iv2 = ir_load(c->f, body, icell, usz);
            IrValue *dat = is_slice ? ir_slice_data(c->f, body, av, elem) : av;
            IrValue *ep  = ir_elem_ptr(c->f, body, dat, iv2, elem);
            ir_store(c->f, body, vcell, ir_load(c->f, body, ep, elem));   // v = arr[i]
            ir_lower_stmts(c, s->as.for_stmt.body);
            ir_lower_for_step(c, head);
            c->loop_head=oh; c->loop_exit=oe; c->loop_defer_mark=om;
            c->loop_step_cell=osc; c->loop_step_ty=ost; c->loop_latch=olt; c->cur = exit;
            break;
        }
        case STMT_MATCH: {
            // Integer/char match on literal + range patterns, or a SUM match on the tag —
            // both lower to the same if-chain, because a sum's discriminant is an
            // ordinary integer (local/internal/design/ir_sum_types.md §3). That is the
            // whole point of not modelling the niche: arm selection is a comparison the
            // numeric domain can reason about.
            Expr *val = s->as.match_stmt.value;
            Type *vt = val ? val->type : NULL;
            IrValue *v = NULL; IrType *sumty = NULL; bool scalar_ok = false;
            IrType *vty = ir_lower_type(c, vt);
            bool string_ok = ir_is_u8_string_type(vty);
            { if (vty && vty->kind == IRT_SUM) sumty = vty;
              // ★ ASK THE LOWERED TYPE, NOT THE AST SHAPE. This read `int_width_cache>0`, so a
              // `bool` scrutinee — not an integer, no width — made the whole function
              // `incomplete`, which SUPPRESSES EVERY PROOF OVER IT. It was the last incomplete
              // function in the corpus (`bool_exhaustive_pass.ln`, spec §15.4). The arm chain
              // below needs nothing special: a scalar arm is an `IR_CMP_EQ`, which a bool
              // supports. Asking the IR type is also the question lowering should ask — what can
              // be matched on is a property of the IR, and re-deriving it from AST fields is how
              // the two drift apart.
              scalar_ok = vty && (vty->kind == IRT_INT || vty->kind == IRT_BOOL); }
            if (!sumty && !scalar_ok && !string_ok) {
                ir_incomplete(c, "a `case` on a scrutinee of this type"); break;
            }
            v = ir_lower_expr(c, val);
            if (sumty && !(v && v->type && v->type->kind==IRT_SUM)) { ir_incomplete(c,"enum-match"); break; }
            IrStrScrut ss = {0};
            if (string_ok) ir_string_scrutinee(c, v, vty, &ss);
            // `case &x` is a NON-CONSUMING match: it borrows the scrutinee for the whole
            // construct. Nothing in the arms reads that borrow, so a liveness-derived region
            // would be empty and `42: x = 99` would look legal — the loan has to be stated.
            IrValue *mborrow = NULL;
            if (s->as.match_stmt.is_borrowed && val && val->kind == EXPR_IDENTIFIER) {
                IrLocal *ml = ir_env_find(c, val->as.identifier_expr.id);
                if (ml && ml->slot) { mborrow = ml->slot; ir_borrow_begin(c->f, c->cur, mborrow); }
            }
            IrValue *tagv = sumty ? ir_sum_tag(c->f, c->cur, v) : NULL;
            IrBlock *join = ir_new_block(c->f);
            StmtMatchCase *elsec = NULL;
            uint64_t matched = 0;          // which variants the arms selected (for `else`)
            for (StmtMatchCase *cs = s->as.match_stmt.cases; cs; cs = cs->next) {
                if (!cs->patterns) { elsec = cs; continue; }
                IrBlock *body = ir_new_block(c->f);
                int bound_k = -1; Expr *bound_pat = NULL;
                for (ExprList *p = cs->patterns; p; p = p->next) {   // OR of this case's patterns
                    Expr *pe = p->expr;
                    IrBlock *nxt = ir_new_block(c->f);
                    if (sumty) {
                        // `Circle(rad)` / `Point` — select on the tag. The payload bindings are
                        // materialised in the BODY, where the tag is known, so IR_SUM_PAYLOAD
                        // is only ever emitted under its own variant.
                        Expr *pv = (pe->kind==EXPR_CALL) ? pe->as.call_expr.callee : pe;
                        Decl *ed = NULL;
                        if (sumty->sname) { Id tn; tn.name=sumty->sname->name; tn.length=sumty->sname->length;
                                            ed = ir_find_enum_decl(c, &tn); }
                        int k = ed ? ir_variant_index(ed, ir_variant_name_of(pv), true) : -1;
                        if (k < 0) {
                            if (pe->line) { ir_cur_line = pe->line; ir_cur_col = pe->col; }
                            ir_incomplete(c, "a pattern that names no variant of the scrutinee"); ir_set_br(c->cur, body); c->cur = nxt; continue;
                        }
                        if (k < 64) matched |= (uint64_t)1 << k;
                        if (pe->kind==EXPR_CALL) { bound_k = k; bound_pat = pe; }
                        IrValue *kc = ir_const_int(c->f, c->cur, k, ir_type_int(c->a,32,true));
                        IrValue *eq = ir_icmp(c->f, c->cur, IR_CMP_EQ, tagv, kc);
                        ir_match_branch(c, sumty, k, pe, v, eq, body, nxt);
                        c->cur = nxt;
                        continue;
                    }
                    if (string_ok) {   // the front end allows only string literals here (E012)
                        if (pe->kind == EXPR_STRING) ir_match_string(c, &ss, pe, body, nxt);
                        else { ir_incomplete(c, "a pattern that is not a string literal"); ir_set_br(c->cur, nxt); }
                        c->cur = nxt;
                        continue;
                    }
                    if (pe->kind == EXPR_RANGE) {
                        Expr *loe=pe->as.range_expr.start, *hie=pe->as.range_expr.end;
                        IrBlock *hitest = ir_new_block(c->f);
                        if (loe){ IrValue *ge=ir_icmp(c->f,c->cur,IR_CMP_SGE,v,ir_lower_expr(c,loe)); ir_set_br_cond(c->cur,ge,hitest,nxt); }
                        else ir_set_br(c->cur, hitest);
                        c->cur = hitest;
                        if (hie){ IrCmp cc=pe->as.range_expr.inclusive?IR_CMP_SLE:IR_CMP_SLT; IrValue *le=ir_icmp(c->f,c->cur,cc,v,ir_lower_expr(c,hie)); ir_set_br_cond(c->cur,le,body,nxt); }
                        else ir_set_br(c->cur, body);
                    } else {
                        IrValue *eq=ir_icmp(c->f,c->cur,IR_CMP_EQ,v,ir_lower_expr(c,pe));
                        ir_set_br_cond(c->cur,eq,body,nxt);
                    }
                    c->cur = nxt;
                }
                IrBlock *ftblk = c->cur;   // where control lands if no pattern matched
                c->cur = body;
                IrLocal *saved = c->locals;             // arm-local payload bindings
                if (bound_k >= 0 && bound_pat) {
                    IrType *pl = (bound_k < sumty->n_fields) ? sumty->fields[bound_k] : NULL;
                    int j=0;
                    for (ExprList *a = bound_pat->as.call_expr.args; a; a = a->next, j++) {
                        Id *bn = a->expr && a->expr->kind==EXPR_IDENTIFIER
                               ? a->expr->as.identifier_expr.id : NULL;
                        if (!bn || !pl || j >= pl->n_fields) continue;
                        // a nested variant NAME tested the payload; binding it here would
                        // shadow the variant with the value it was testing for
                        if (ir_nested_variant_index(c, pl->fields[j], a->expr) >= 0) continue;
                        IrValue *pvv = ir_sum_payload(c->f, c->cur, v, bound_k, j, pl->fields[j]);
                        ir_env_add(c, bn, NULL, pvv);   // a read-only binding, not a slot
                    }
                }
                ir_lower_stmts(c, cs->body);
                c->locals = saved;
                if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, join);
                c->cur = ftblk;
            }
            if (elsec) {
                // ★ NARROWING IN THE `else` ARM. Once every other variant has its own arm, the
                // scrutinee here can only be the remaining one — so `case r { NotFound: … 
                // Denied: … else: printf("%s", r) }` means the PAYLOAD, and passing the whole
                // sum printed `(null)`. Same rule as `if x` over an optional, arrived at from
                // the other side: exclusion is narrowing too. Only when exactly ONE variant is
                // left and it carries a payload; anything else is still the sum.
                IrLocal *esaved = c->locals;
                if (sumty && val && val->kind==EXPR_IDENTIFIER && sumty->n_fields <= 64) {
                    int rem = -1, nrem = 0;
                    for (int q=0; q<sumty->n_fields; q++)
                        if (!((matched >> q) & 1u)) { rem = q; nrem++; }
                    if (nrem == 1 && rem >= 0 && sumty->fields[rem])
                        ir_env_add(c, val->as.identifier_expr.id, NULL,
                                   ir_sum_optional_payload(c, v, rem));
                }
                ir_lower_stmts(c, elsec->body);
                c->locals = esaved;
            }
            if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, join);
            c->cur = join;
            if (mborrow) ir_borrow_end(c->f, c->cur, mborrow);   // the scoped loan ends here
            break;
        }
        case STMT_BREAK: case STMT_CONTINUE: {
            // Both LEAVE the loop body, so both run the defers it registered — in reverse,
            // like any other block exit. Without this a `continue` skipped them entirely.
            // Inside a deferred statement the loop is inside it too (E138), so its mark is above
            // the statement's own and this replays only what that loop's body registered.
            if (c->ndefers > c->loop_defer_mark) ir_lower_replay_defers(c, c->loop_defer_mark);
            IrBlock *tgt = (s->kind==STMT_BREAK) ? c->loop_exit : ir_loop_continue_target(c);
            if (tgt) ir_set_br(c->cur, tgt);
            break;
        }
        case STMT_ASSERT: {
            // The two IR primitives the language could not previously reach. An `assert` is an
            // OBLIGATION the numeric analysis must discharge (and reports if it cannot); an
            // `assume` is a GIVEN it may use. Same node, opposite directions.
            IrValue *cv = ir_lower_expr(c, s->as.assert_stmt.cond);
            if (cv) { if (s->as.assert_stmt.is_assume) ir_assume_trusted(c->f, c->cur, cv);
                      else                             ir_assert(c->f, c->cur, cv); }
            break;
        }
        case STMT_UNSAFE: {
            // Everything emitted for the body is UNCHECKED. `unsafe` is lexical, so the region
            // is "this block from here on, plus every block created while inside" — marking
            // only the arithmetic missed the STORE that narrows it, which is where Path-F puts
            // the obligation, and the corpus's own `unsafe { var c u8 = a + b }` still fired.
            bool o=c->unsafe; c->unsafe=true;
            IrBlock *b0 = c->cur; IrInstr *t0 = b0 ? b0->instrs_tail : NULL;
            bool b0_set = b0 && ir_is_set_term(b0);
            int32_t nb0 = c->f->next_block_id;
            ir_lower_stmts(c, s->as.unsafe_stmt.body);
            for (IrInstr *i = (t0 ? t0->next : (b0 ? b0->instrs : NULL)); i; i = i->next) i->unchecked = true;
            for (IrBlock *b = c->f->blocks; b; b = b->next)
                if (b->id >= nb0) for (IrInstr *i = b->instrs; i; i = i->next) i->unchecked = true;
            // ...and every TERMINATOR the body set: a `return` inside the block narrows its value
            // into the return type there, and that narrowing is waived like the others. A
            // terminator still unset belongs to code after the block.
            if (b0 && !b0_set && ir_is_set_term(b0)) b0->term.unchecked = true;
            for (IrBlock *b = c->f->blocks; b; b = b->next)
                if (b->id >= nb0 && ir_is_set_term(b)) b->term.unchecked = true;
            c->unsafe=o; break;
        }
        // ── comptime `if`: the FRONT END already chose, and lowering must honour the choice ──
        // Dead-branch elimination, decided by sema (`is_taken`) and mirrored by the old emitter
        // since it existed. The IR had no case at all, so the statement fell into the default
        // below and made the whole function `incomplete` — which is fail-closed and therefore
        // sound, but it meant the SELECTED BRANCH WAS ABSENT FROM THE IR. The new backend then
        // faithfully emitted a program missing a line the old one printed (`emit_gate`'s
        // `comptime_if_pass`: old prints "Running on Linux\nDone", new prints "Done").
        //
        // It is not a codegen bug and it was never in the emitter: an IR that claims to be the
        // semantic authority cannot omit a statement the program executes. Lowering the taken
        // branch INLINE is also the honest representation — after the choice there is no
        // branch left, which is exactly what `comptime` means.
        case STMT_COMPTIME_IF: {
            if (!s->as.comptime_if_stmt.evaluated) {   // sema must have decided; say so if not
                ir_incomplete(c, "comptime-if-unevaluated"); break;
            }
            StmtList *taken = s->as.comptime_if_stmt.is_taken
                            ? s->as.comptime_if_stmt.then_body
                            : s->as.comptime_if_stmt.else_branch;   // NULL else ⇒ nothing at all
            if (taken) ir_lower_stmts(c, taken);
            break;
        }
        // `use path as name` in a function body: implementation-defined in spec 16, and this
        // implementation does not model it (the alias resolved to a C name, an AST-emitter
        // leftover). Refused at the statement (ir_emit_refuse_opaque).
        case STMT_USE: ir_incomplete(c, "a `use` statement in a function body"); break;
        default: ir_incomplete(c, "unhandled-stmt"); break;
    }
}
// `defer` is BLOCK-scoped, not function-scoped. The old backend emits the deferred call at
// the end of the block that registered it — `defer printf("2")` inside an `if` prints before
// the statement after the `if` — and the lowering was written on the opposite assumption, so
// every nested defer ran too late and in the wrong order (`12-3` became `1-23`, and a defer
// inside a loop body ran once at the end instead of once per iteration).
//
// A block records the stack depth it started at, replays what IT registered on the way out,
// and pops back. A `return` still flushes EVERYTHING (an exit leaves every scope at once), so
// the replay here is skipped when control has already left.
static void ir_lower_stmts(LowerCtx *c, StmtList *body) {
    int mark = c->ndefers;
    for (StmtList *b = body; b && !ir_is_set_term(c->cur); b = b->next) ir_lower_stmt(c, b->stmt);
    if (c->ndefers > mark && !ir_is_set_term(c->cur)) ir_lower_replay_defers(c, mark);
    if (c->ndefers > mark) c->ndefers = mark;
}

// Emit `assume(param OP const)` for a scalar param's refinement (`n u32 < 4096`).
// This is where a front-end refinement ENTERS the IR as a verifiable fact — the IR
// then owns it; no analysis re-reads the AST. (Sovereignty: lowering is the adapter.)
static void ir_lower_param_refinements(LowerCtx *c, IrValue *pv, Type *pty, Decl *pdecl) {
    if (!pv || !pv->type || pv->type->kind != IRT_INT || !pdecl) return;
    (void)pty;
    bool sgn = pv->type->is_signed;
    for (ExprList *cn = pdecl->as.variable_decl.constraints; cn; cn = cn->next) {
        Expr *con = cn->expr;
        if (!con || con->kind != EXPR_BINARY) continue;
        Expr *rhs = con->as.binary_expr.right;
        IrCmp cmp;
        // `b i32 != 0` — a DISEQUALITY refinement. It was skipped along with `==`, so the
        // commonest precondition in the language (`!= 0` on a divisor) never reached the IR at
        // all. The numeric domain cannot hold a hole in an interval, but the IR can still
        // STATE the fact, and the division check reads it — the same shape as a guard, said at
        // the entry instead of on an edge.
        if (con->as.binary_expr.op == TOKEN_BANG_EQUAL) {
            IrValue *rvn = ir_lower_refinement_rhs(c, rhs, pv->type);
            if (rvn && rvn->type && rvn->type->kind==IRT_INT)
                ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_NE, pv, rvn));
            continue;
        }
        if (con->as.binary_expr.op == TOKEN_EQUAL_EQUAL) {           // `n u32 == 4`: the dual of
            IrValue *rve = ir_lower_refinement_rhs(c, rhs, pv->type);   // the call-site EQ assert
            if (rve && rve->type && rve->type->kind==IRT_INT)
                ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_EQ, pv, rve));
            continue;
        }
        if (!ir_tok_cmp(con->as.binary_expr.op, sgn, &cmp)) continue;
        IrValue *rv = ir_lower_refinement_rhs(c, rhs, pv->type);   // literal / a.len / param
        if (rv && rv->type && rv->type->kind==IRT_INT)
            ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, pv, rv));
    }
}


// ── entry: lower one function ────────────────────────────────────────────────
IrFunc *ir_lower_function(Decl *fn, DeclList *globals, Arena *a) {
    IrType tmp; LowerCtx cc = {0}; cc.a = a; cc.globals = globals; (void)tmp;
    Id *fnm = fn->as.function_decl.name;
    IrFunc *f = ir_func_new(a, ir_qualified_name(a, fn, fnm), NULL,
                            fn->kind==DECL_FUNCTION ? IR_FUNC_PURE : IR_FUNC_PROC);
    f->src_decl = fn;   // opaque provenance (void*) — the IR never derefs it
    if (fnm) f->src_name = ir_intern(a, fnm->name, fnm->length);   // as written, for diagnostics
    // The exception to "everything terminates", written by the function that wants it. The
    // effect row already had a `diverge` bit and a parser for it; this is the one place that
    // had to start reading it, so the opt-out needed no new syntax.
    // E.1: the ROW grants divergence, and only the row (`@diverges` was removed, I.67).
    f->is_cold      = fn->as.function_decl.is_cold;
    f->is_hot       = fn->as.function_decl.is_hot;
    f->is_noreturn  = fn->as.function_decl.is_noreturn;
    f->is_allocator = fn->as.function_decl.is_allocator;
    f->may_diverge = fn->as.function_decl.effects_declared
                  && (fn->as.function_decl.effects_bound & EFFECT_DIVERGE);
    cc.fdecl = fn;      // for callee-side return-ensures asserts
    cc.f = f; cc.cur = f->entry;
    // ★ THE POSITION CURSOR IS GLOBAL, so it must be re-anchored per function. Instructions take
    // their line from `ir_cur_line`, which only statements and expressions advance — so the
    // parameter spills at the top of a function inherited whatever the PREVIOUSLY lowered
    // function left there. `func ignore(T type, mov x T) { }` instantiated for a linear T
    // reported its unconsumed `x` (E003) at `return 0` in main, a line of a different function;
    // the concrete spelling had no position at all. The function's header is where it starts,
    // and each parameter's own declaration is where an obligation on it originates.
    if (fn->line) { ir_cur_line = fn->line; ir_cur_col = fn->col; }
    f->ret_type = ir_lower_borrow_binding_type(&cc, fn->as.function_decl.return_type);
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next) {
        if (!p->decl) continue;
        if (p->decl->line) { ir_cur_line = p->decl->line; ir_cur_col = p->decl->col; }
        // A DESTRUCTURING parameter — `close_file(mov {handle} File)` — is one parameter that
        // binds field names directly. It was dropped ENTIRELY: the function lowered with no
        // parameters at all while its call sites still passed an argument, so the IR
        // disagreed with itself on arity and no pass could judge what the call transferred
        // (the ownership analysis had to guess, and guessed permissively). Lower it as the
        // ordinary owned struct parameter it is, with each name bound to its field.
        if (p->decl->kind == DECL_DESTRUCT) {
            Type   *dty = p->decl->as.destruct_decl.type;
            IrType *dt  = ir_lower_type(&cc, dty);
            IrValue *pv = ir_add_param(f, dt, NULL);
            IrValue *slot = ir_alloca(f, cc.cur, dt);
            pv->owns = (dty && dty->mode == MODE_OWNED);
            // ...but the SLOT does not own it. Lain's rule for a destructuring parameter is
            // that taking the value APART is the disposal — `func drop(mov {id} Resource) { }`
            // is the idiomatic destructor, and both corpus tests say so in their comments
            // ("destructure to consume"). The struct has no existence as a unit past the
            // pattern, so there is no struct-level obligation to discharge.
            //
            // KNOWN GAP, and it is the old engine's too: the obligation does not TRANSFER to
            // the field bindings the way a sum payload's does, so an owned field named by the
            // pattern and then dropped on the floor is not reported. Closing it is a language
            // decision (it would make the two corpus tests fail-tests), not a lowering one.
            slot->owns = false;
            ir_store(f, cc.cur, slot, pv);
            for (IdList *nm = p->decl->as.destruct_decl.names; nm; nm = nm->next) {
                IrType *fty = NULL;
                int idx = ir_field_index(dt, nm->id, &fty);
                if (idx < 0) { ir_incomplete(&cc, "destructure-field"); continue; }
                ir_env_add(&cc, nm->id, ir_field_ptr(f, cc.cur, slot, idx, fty), NULL);
            }
            continue;
        }
        if (p->decl->kind != DECL_VARIABLE) continue;
        Type   *pty = p->decl->as.variable_decl.type;
        IrType *pt  = ir_lower_type(&cc, pty);
        Id     *pnm = p->decl->as.variable_decl.name;
        IrName *pin = pnm ? ir_intern(a, pnm->name, pnm->length) : NULL;   // IR-owned param name
        // OWNERSHIP of the binding (IrValue.owns): a parameter owns its argument only when
        // the declaration transfers it. A borrow — shared or mutable — does not, and must
        // never be leak-checked or consumed by the callee. This is the declared mode, the
        // same category of front-end fact as the type itself; what the IR does with it is
        // decided by analysis, not asked of the front end.
        bool p_owns = pty && pty->mode == MODE_OWNED;
        bool mut_addr = ir_mut_by_address(pt) && pty && pty->mode == MODE_MUTABLE;
        bool shr_addr = ir_shared_agg_by_address(pt, pty);
        if (mut_addr || shr_addr) {
            // A BORROW OF A COPIED TYPE TRAVELS AS AN ADDRESS, mutable or shared.
            //   mutable — so the callee's writes reach the caller's storage;
            //   shared  — so no copy is made (P1), and so the backend has a POINTER parameter
            //             to attach `nonnull` and `access(read_only, n, m)` to.
            // Either way the pointer IS the slot. (A slice/array/pointer already shares its
            // data, so it stays by value; a scalar is cheaper in a register.)
            IrType *ptr = ir_type_new(cc.a, IRT_PTR); ptr->elem = pt;
            ptr->ptr_mut = mut_addr;     // a shared borrow is a pointer to CONST
            ptr->borrowed = true;        // a REFERENCE into the caller's storage — see below
            IrValue *pv = ir_add_param(f, ptr, pin);
            pv->owns = false;                       // a borrow never owns, shared or mutable
            ir_env_add(&cc, pnm, pv, NULL);
        } else if (pt->kind == IRT_STRUCT) {
            // a by-value struct param: materialize to a slot so its fields address
            IrValue *pv = ir_add_param(f, pt, pin);
            IrValue *slot = ir_alloca(f, cc.cur, pt);
            pv->owns = slot->owns = p_owns;         // the home slot inherits the binding's mode
            ir_store(f, cc.cur, slot, pv);
            ir_env_add(&cc, pnm, slot, NULL);
        } else {
            IrValue *pv = ir_add_param(f, pt, pin);
            pv->owns = p_owns;
            ir_env_add(&cc, pnm, NULL, pv);
        }
    }
    // second pass (all params now in scope, so RHS `m`/`a.len` resolve): scalar refinements
    // (`n < 4096`, `i < a.len`) and dependent-length constraints (`out i32[m]`) → entry assumes.
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Type *pty = p->decl->as.variable_decl.type;
        IrLocal *l = ir_env_find(&cc, p->decl->as.variable_decl.name);
        IrValue *pv = l ? l->param : NULL;
        if (pv && pv->type && pv->type->kind==IRT_INT)
            ir_lower_param_refinements(&cc, pv, pty, p->decl);
        if (pv && pty && pty->kind==TYPE_ARRAY && pty->array_len<0 && pty->size_expr) {
            ir_lower_slice_len_refinement(&cc, pv, pty);
            ir_lower_region_shape(&cc, pv, pty);      // S2: rank-N shape when len is a product
        }
    }
    // B5: record only the SIGNATURE fact — this function returns a reference, so its result
    // borrows something of the caller's. WHICH parameter is a body fact, inferred later by
    // analysis/borrow.h (see IrFunc.ret_borrow_mask). Lowering does not analyse.
    f->has_decreasing = (fn->as.function_decl.decreasing_measure != NULL);
    { // WHETHER the return is a reference is now on the TYPE (`ret_type->borrowed`, set by
      // ir_lower_borrow_binding_type), so there is nothing to record here but the annotation.
      int bi = ir_param_index_by_name(fn, fn->as.function_decl.ret_borrow_of);
      if (bi >= 0 && bi < 64) { f->ret_borrow_annot = true; f->ret_borrow_annot_mask = 1ull<<bi; } }
    ir_lower_stmts(&cc, fn->as.function_decl.body);
    if (!ir_is_set_term(cc.cur)) {
        ir_lower_flush_defers(&cc);   // falling off the end is an exit too
        // ★ FALLING OFF A FUNCTION THAT RETURNS A VALUE IS A PATH THE FRONT END PROVED ABSENT
        // (I.117): every path returns, and a `case` without `else:` covers its scrutinee. It was
        // closed with a valueless `return` (a C constraint violation in a non-void function), so a
        // WRONG proof returned garbage in silence: I.105's `case` missing an arm returned 0 past C
        // at -O0 and -O2, UBSan and the interpreter. Unreachable says what was proved; the
        // interpreter checks it (PROOF FAILED) and the C states it as __builtin_unreachable(),
        // which UBSan reports. `main` keeps C's implicit `return 0`.
        bool is_main = f->name && f->name->length == 4 && strncmp(f->name->name, "main", 4) == 0;
        bool valued  = f->ret_type && f->ret_type->kind != IRT_UNIT && f->ret_type->kind != IRT_NEVER;
        if (!ir_is_set_term(cc.cur)) {   // unless a defer ended it
            if (valued && !is_main) ir_set_unreachable(cc.cur);
            else ir_set_ret(cc.cur, NULL);
        }
    }
    // ★ `[noreturn]` IS AN OBLIGATION. It becomes `__attribute__((noreturn))`, and returning from
    // such a function is undefined behaviour: `[noreturn] func f(x i32) i32 { return x }` was
    // accepted, and at -O2 its caller ran off the end of the code into a segfault — a program
    // with no `unsafe` in it. Every return of a [noreturn] function asserts false (E135): one
    // the analysis proves unreachable costs nothing, any other is refused where it is.
    if (f->is_noreturn) {
        for (IrBlock *b = f->blocks; b; b = b->next) {
            if (b->term.kind != IR_TERM_RET) continue;
            if (b->term.line) { ir_cur_line = b->term.line; ir_cur_col = b->term.col; }
            else if (fn->line) { ir_cur_line = fn->line; ir_cur_col = fn->col; }
            IrValue *z = ir_const_int(f, b, 0, ir_type_int(a, 32, false));
            ir_assert_coded(f, b, ir_icmp(f, b, IR_CMP_NE, z, z), 135);
        }
    }
    ir_finalize_cfg(f);
    return f;
}

// Lower a whole program to a module IrFunc list: every func/proc body, PLUS a
// declaration-only stub (is_extern, no body) for each extern func/proc so an
// interprocedural pass (effects, and later borrow/linearity) can classify calls to them
// WITHOUT consulting the AST — the module is self-contained. Returns the list head.
static IrSAExpr *ir_lower_static_expr(LowerCtx *c, Expr *e) {
    if (!e) return NULL;
    if (e->kind == EXPR_CAST) return ir_lower_static_expr(c, e->as.cast_expr.expr);
    if (e->kind == EXPR_IDENTIFIER && e->decl && e->decl->kind == DECL_VARIABLE &&
        !e->decl->as.variable_decl.is_mutable && e->decl->as.variable_decl.init &&
        c->const_depth < 16) {                         // a named constant: its initialiser
        c->const_depth++;
        IrSAExpr *x = ir_lower_static_expr(c, e->decl->as.variable_decl.init);
        c->const_depth--;
        return x;
    }
    IrSAExpr *x = arena_push_aligned(c->a, IrSAExpr);
    memset(x, 0, sizeof *x);
    switch (e->kind) {
        case EXPR_LITERAL: x->kind = IR_SA_CONST; x->value = e->as.literal_expr.value; break;
        case EXPR_CHAR:    x->kind = IR_SA_CONST; x->value = (unsigned char)e->as.char_expr.value; break;
        case EXPR_BUILTIN:
            x->kind = e->as.builtin_expr.builtin_kind == BUILTIN_SIZEOF ? IR_SA_SIZEOF : IR_SA_ALIGNOF;
            x->type = ir_lower_type(c, e->as.builtin_expr.vec_type);
            break;
        case EXPR_UNARY: {
            // `~` of an UNSIGNED N-bit value is 2^N - 1 - x (sema's sa_eval says why), and the C
            // here computes in `long long`, where `~0` is -1: write the width in.
            Type *ot = e->as.unary_expr.op == TOKEN_TILDE ? e->as.unary_expr.right->type : NULL;
            while (ot && ot->kind == TYPE_COMPTIME) ot = ot->element_type;
            int ub = 0; bool us = true;
            if (ot && parse_iN_uN(ot, &ub, &us) && !us && ub >= 1 && ub < 64) {
                IrSAExpr *m = arena_push_aligned(c->a, IrSAExpr);
                memset(m, 0, sizeof *m);
                m->kind = IR_SA_CONST; m->value = (int64_t)((((uint64_t)1) << ub) - 1);
                x->kind = IR_SA_BINARY; x->op = "-";
                x->l = m;
                x->r = ir_lower_static_expr(c, e->as.unary_expr.right);
                break;
            }
            x->kind = IR_SA_UNARY;
            x->op = e->as.unary_expr.op == TOKEN_MINUS ? "-" : e->as.unary_expr.op == TOKEN_BANG ? "!" : "~";
            x->l = ir_lower_static_expr(c, e->as.unary_expr.right);
            break;
        }
        case EXPR_BINARY: {
            x->kind = IR_SA_BINARY;
            switch (e->as.binary_expr.op) {
                case TOKEN_KEYWORD_AND: x->op = "&&"; break;
                case TOKEN_KEYWORD_OR:  x->op = "||"; break;
                default: x->op = token_kind_to_str(e->as.binary_expr.op); break;   // + - * / % & | ^ == != < <= > >=
            }
            x->l = ir_lower_static_expr(c, e->as.binary_expr.left);
            x->r = ir_lower_static_expr(c, e->as.binary_expr.right);
            break;
        }
        default: x->kind = IR_SA_CONST; x->value = 0; break;   // sema refused anything else (E133)
    }
    return x;
}

// Does a module assert MEASURE a type? Only such an assert reaches the C: sema decided every
// other one (E134 when false), and re-checking it in C's `long long` arithmetic can only
// disagree. It did: `assert ~(0 as u32) == 4294967295`, true in Lain, failed as `~0LL == ...`.
static bool ir_sa_has_layout(const IrSAExpr *x) {
    if (!x) return false;
    if (x->kind == IR_SA_SIZEOF || x->kind == IR_SA_ALIGNOF) return true;
    return ir_sa_has_layout(x->l) || ir_sa_has_layout(x->r);
}
static IrFunc *ir_lower_module(DeclList *program, Arena *a) {
    IrFunc *head=NULL, *tail=NULL;
    // DECIDE-O: the module-scope asserts, in source order, for the emitter. Reset first: the
    // driver may lower a module more than once.
    ir_static_asserts = NULL;
    ir_data_objects = NULL; ir_data_n = 0;             // module constant tables, made on first use
    { IrStaticAssert **sa_tail = &ir_static_asserts;
      LowerCtx sc = {0}; sc.a = a; sc.globals = program;
      for (DeclList *d = program; d; d = d->next) {
          if (!d->decl || d->decl->kind != DECL_STATIC_ASSERT) continue;
          IrSAExpr *cond = ir_lower_static_expr(&sc, d->decl->as.static_assert_decl.cond);
          if (!ir_sa_has_layout(cond)) continue;          // decided by sema, not by C
          IrStaticAssert *s = arena_push_aligned(a, IrStaticAssert);
          s->cond = cond;
          s->line = d->decl->line; s->col = d->decl->col; s->next = NULL;
          *sa_tail = s; sa_tail = &s->next;
      } }
    for (DeclList *d = program; d; d = d->next) {
        if (!d->decl) continue;
        IrFunc *f = NULL;
        DeclKind k = d->decl->kind;
        // A generic TEMPLATE has no runtime existence — only its instances do, and sema has
        // already appended those to this same list. Lowering the template anyway produced a
        // function whose type parameter is a `unit` VALUE parameter, i.e. `void* pick(void,
        // void*, void*)`, which no C compiler accepts: every generic program in the corpus
        // failed to build for this one reason. (It is also unanalysable by construction —
        // `T` has no representation — so every proof over it was noise.)
        if (decl_is_generic_template(d->decl)) continue;
        // ...and an EMPTY body is still a body. `func drop(mov {id} Resource) { }` — the
        // idiomatic Lain destructor — parses to a NULL statement list, so requiring one
        // skipped the function ENTIRELY: no definition in the emitted C, and every call to it
        // an undefined symbol at link time. A Lain function always has a body (an extern is a
        // different DeclKind), so the presence of statements is not the question.
        if (k==DECL_FUNCTION) {
            f = ir_lower_function(d->decl, program, a);
        } else if (k==DECL_EXTERN_FUNCTION) {
            Id *nm = d->decl->as.function_decl.name;
            f = ir_func_new(a, nm ? ir_intern(a, nm->name, nm->length) : NULL, NULL,
                            k==DECL_EXTERN_FUNCTION ? IR_FUNC_PURE : IR_FUNC_PROC);
            f->is_extern = true; f->src_decl = d->decl;
            f->is_cold      = d->decl->as.function_decl.is_cold;       // believed, as the row is
            f->is_hot       = d->decl->as.function_decl.is_hot;
            f->is_allocator = d->decl->as.function_decl.is_allocator;
            f->is_noreturn  = d->decl->as.function_decl.is_noreturn;
            // E.5: carry the DECLARED row across the seam. Mapped bit by bit on purpose — the
            // two enums are parallel today, and a silent `(IrEffect)eff` would turn any future
            // divergence between them into a wrong effect rather than a compile error.
            if (d->decl->as.function_decl.effects_declared) {
                EffectSet e = d->decl->as.function_decl.effects_bound;
                IrEffect r = 0;
                if (e & EFFECT_DIVERGE) r |= IR_EFFECT_DIVERGE;
                if (e & EFFECT_RAISES)  r |= IR_EFFECT_RAISES;
                if (e & EFFECT_IO)      r |= IR_EFFECT_IO;
                if (e & EFFECT_ALLOC)   r |= IR_EFFECT_ALLOC;
                f->declared_row = r; f->has_declared_row = true;
            }
            f->is_variadic = d->decl->as.function_decl.is_variadic;
            // An extern returning a REFERENCE lends something of the caller's, exactly as a
            // Lain function does — and this fact was recorded only in ir_lower_function, which
            // runs for BODIES. So `extern func pick(a var Data) var i32` lent nothing and every
            // conflict on its result was invisible, while the identical signature WITH a body
            // was caught. The mask cannot be inferred here (there is no body to read), so the
            // fallback is every reference parameter — the sound direction, and the concrete
            // place where a LIFETIME ANNOTATION would buy precision (Stage V F1).
            { int bi = ir_param_index_by_name(d->decl, d->decl->as.function_decl.ret_borrow_of);
              if (bi >= 0 && bi < 64) { f->ret_borrow_annot = true; f->ret_borrow_annot_mask = 1ull<<bi; } }
            // An extern was lowered as a NAME and nothing else — no return type, no
            // parameters. Analyses that ask what a call transfers got no answer, and the new
            // emitter could not declare it, so every call to one was an implicit declaration
            // in the generated C: 102 of the corpus's 141 backend build failures, one cause.
            { LowerCtx ec = {0}; ec.a = a; ec.globals = program; ec.f = f; ec.cur = f->entry;
              f->ret_type = ir_lower_borrow_binding_type(&ec, d->decl->as.function_decl.return_type);
              for (DeclList *p = d->decl->as.function_decl.params; p; p = p->next) {
                  if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
                  Type *pty = p->decl->as.variable_decl.type;
                  Id   *pnm = p->decl->as.variable_decl.name;
                  IrType *pt = ir_lower_type(&ec, pty);
                  // ★ A `var` PARAMETER OF A COPIED TYPE IS AN ADDRESS, BODY OR NOT (I.115). The
                  // call passes the caller's slot, as it does to a bodied function, but the
                  // extern was declared by value: `frexp(8.0, var e)` handed `double frexp(double,
                  // int32_t)` the address of e, and frexp wrote through its low 32 bits. Every C
                  // out-parameter (frexp, modf, strtol's end) crashed, and only the wrong spelling,
                  // `bump(n)`, gave well-typed C. A shared borrow stays by value: that is how C
                  // takes a struct.
                  if (ir_mut_by_address(pt) && pty && pty->mode == MODE_MUTABLE) {
                      IrType *ptr = ir_type_new(a, IRT_PTR); ptr->elem = pt;
                      ptr->ptr_mut = true; ptr->borrowed = true;
                      pt = ptr;
                  }
                  IrValue *pv = ir_add_param(f, pt, pnm ? ir_intern(a, pnm->name, pnm->length) : NULL);
                  pv->owns = pty && pty->mode == MODE_OWNED;
              } }
        }
        if (f) { if (!head) head=tail=f; else { tail->next=f; tail=f; } }
    }
    return head;
}

#endif // LAIN_IR_LOWER_H
