// interp.h — THE CONCRETE SEMANTICS OF LAIN-IR, EXECUTABLE (DECIDE-W, endgame step 1).
//
// One definition of what a program means, written once: this file. It is used as a TEST ORACLE
// first (`lain --interpret`, compared with the C the backend emits), and it is the specification
// the analyses are abstractions of. Two properties make it more than a second backend:
//
//  · VALUES ARE ABSTRACT. A sum is a tag and its payload, a struct its fields, an array its
//    elements; nothing here knows a niche, a [packed] container or a field order. So a disagreement
//    with the emitted C on the same program is a fact about the BACKEND's representation choices —
//    the class (D-62, the half-size f64 vector) that behaviour-based gates kept missing.
//
//  · EVERY PROOF IS CHECKED AS IT IS USED. The analyses accepted the program, so each obligation
//    they discharged must hold on every execution: an operation proven free of overflow does not
//    overflow, a proven index is in bounds, a divisor is not zero, an `assert` holds, an assume the
//    compiler established holds, a value is initialised when read, a pointer's storage is alive.
//    When one fails here, a proof was false: the interpreter stops and names the site. That is a
//    sanitizer that sees what ASan and UBSan cannot (an overflow inside a struct's array field, a
//    read past a slice's length that stays inside the underlying array), at the IR's own level.
//
// Exit status: the program's own (main's value & 255, `exit`'s code, 134 for a panic), or
//   99  a proof failed (soundness bug) — or a TRUSTED assumption was violated (a lying contract)
//   96  undefined behaviour inside `unsafe` (the programmer's assertion, not a proof, was false)
//   98  the interpreter does not model something here yet (coverage, not a verdict)
//   97  the step budget ran out (LAIN_INTERP_STEPS, default 200M)
#ifndef LAIN_IR_INTERP_H
#define LAIN_IR_INTERP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include "ir.h"
#include "layout.h"

typedef enum { IV_UNINIT, IV_INT, IV_FLT, IV_PTR, IV_SLICE, IV_AGG, IV_SUM, IV_FUNC, IV_UNIT } IvKind;

typedef struct IObj IObj;
#define IP_MAX 12
// A pointer: an object and a PATH into its value tree (each step an element or field index). The
// LAST step may move (ELEM_PTR); `lo`/`hi` bound it — a slice's data pointer carries the slice's
// own bounds, so a read past the slice is caught even inside the array it views. hi < 0: the
// enclosing aggregate's length.
typedef struct { IObj *o; int8_t d; int32_t p[IP_MAX]; int32_t lo, hi; } IPtr;

typedef struct IVal {
    uint8_t  k;
    int32_t  n;            // IV_AGG: element count; IV_SUM: the tag
    uint64_t i;            // IV_INT: the value's two's-complement bits (the type says how to read)
    double   f;            // IV_FLT
    IPtr     p;            // IV_PTR, IV_SLICE's data
    int64_t  len;          // IV_SLICE
    struct IVal *e;        // IV_AGG: fields / elements / vector lanes; IV_SUM: payload fields
    IrName  *fn;           // IV_FUNC
} IVal;

struct IObj { IVal root; bool live, ro, heap, raw; const char *what; struct IObj *frame_next;
              int32_t seal; };   // an immutable binding's storage: written only by its seal (ir.h)

typedef struct IFrame { IrFunc *f; IVal *v; int nv; IObj *objs; struct IFrame *up; } IFrame;

typedef struct IState {
    IrFunc *mod;
    long long steps, budget;
    jmp_buf stop;
    int status;
    const char *file;
    const char *who;          // the failure prefix: `lain --interpret`, or compile-time evaluation
    bool budget_is_callers;   // the caller reports an exhausted budget itself, with its total
} IState;
static IState *ist = NULL;

// ── stopping ─────────────────────────────────────────────────────────────────────────────
static IrFunc *ii_cur_f = NULL;
static void ii_stop(int status) __attribute__((noreturn));
static void ii_stop(int status) { ist->status = status; longjmp(ist->stop, 1); }
static void ii_fail(int status, const char *kind, IrInstr *at, const char *fmt, ...) __attribute__((noreturn));
// A caller that classifies failures itself (the soundness harness) sets ii_quiet and reads ii_why,
// the last failure's message: its kind, then what happened ("PROOF FAILED: division by zero ...").
static bool ii_quiet = false;
static char ii_why[256];
static void ii_fail(int status, const char *kind, IrInstr *at, const char *fmt, ...) {
    if (status == 97 && ist->budget_is_callers) ii_stop(status);
    int n = snprintf(ii_why, sizeof ii_why, "%s", kind);
    if (fmt && n >= 0 && n < (int)sizeof ii_why) {
        va_list ap; va_start(ap, fmt);
        snprintf(ii_why + n, sizeof ii_why - (size_t)n, ": ");
        vsnprintf(ii_why + n + 2, sizeof ii_why - (size_t)n - 2, fmt, ap); va_end(ap);
    }
    if (ii_quiet) ii_stop(status);
    fprintf(stderr, "%s: %s", ist->who ? ist->who : "lain --interpret", kind);
    if (at && at->line) fprintf(stderr, " at %s:%lld:%lld", ist->file ? ist->file : "?",
                                (long long)at->line, (long long)at->col);
    if (ii_cur_f && ii_cur_f->name) fprintf(stderr, " (in %.*s)", (int)ii_cur_f->name->length, ii_cur_f->name->name);
    if (fmt) { va_list ap; va_start(ap, fmt); fputs(": ", stderr); vfprintf(stderr, fmt, ap); va_end(ap); }
    fputc('\n', stderr);
    ii_stop(status);
}
#define II_PROOF(at, ...)   ii_fail(99, "PROOF FAILED", at, __VA_ARGS__)
#define II_UNSUP(at, ...)   ii_fail(98, "NOT MODELLED", at, __VA_ARGS__)
#define II_UB(at, ...)      ii_fail(96, "UNDEFINED BEHAVIOUR IN UNSAFE CODE", at, __VA_ARGS__)

// ── integers: the mathematical value, and what a type does to it ──────────────────────────
static int  it_bits(const IrType *t) { if (!t) return 64; if (t->kind == IRT_BOOL) return 1; return t->bits > 0 ? t->bits : 32; }
static bool it_signed(const IrType *t) { return t && t->kind == IRT_INT && t->is_signed; }
static __int128 it_min(const IrType *t) { int b = it_bits(t); return it_signed(t) ? -((__int128)1 << (b - 1)) : 0; }
static __int128 it_max(const IrType *t) { int b = it_bits(t); return it_signed(t) ? ((__int128)1 << (b - 1)) - 1 : (((__int128)1 << b) - 1); }
static bool it_fits(__int128 x, const IrType *t) { return x >= it_min(t) && x <= it_max(t); }
static __int128 it_wrap_bits(__int128 x, int b, bool sgn) {
    unsigned __int128 u = (unsigned __int128)x;
    if (b < 128) u &= (((unsigned __int128)1) << b) - 1;
    if (sgn && b > 0 && ((u >> (b - 1)) & 1)) return (__int128)u - ((__int128)1 << b);
    return (__int128)u;
}
static __int128 it_wrap(__int128 x, const IrType *t) { return it_wrap_bits(x, it_bits(t), it_signed(t)); }
static __int128 it_clamp(__int128 x, const IrType *t) { __int128 lo = it_min(t), hi = it_max(t); return x < lo ? lo : x > hi ? hi : x; }

static void iv_int(IVal *v, __int128 x) { memset(v, 0, sizeof *v); v->k = IV_INT; v->i = (uint64_t)x; }
static __int128 iv_get(const IVal *v, const IrType *t, IrInstr *at) {
    if (v->k == IV_UNINIT) II_PROOF(at, "read of an uninitialised value");
    if (v->k == IV_PTR || v->k == IV_FUNC) II_UNSUP(at, "a pointer used as an integer");
    if (v->k != IV_INT) II_UNSUP(at, "an integer operand of kind %d", v->k);
    bool uns = t && ((t->kind == IRT_INT && !t->is_signed) || t->kind == IRT_BOOL);
    return uns ? (__int128)v->i : (__int128)(int64_t)v->i;
}
static bool iv_is_null(const IVal *v) { return v->k == IV_INT && v->i == 0; }

// ── allocation: frame temporaries are released with their frame; object trees with the object ──
typedef struct IAlloc { void *p; struct IAlloc *next; } IAlloc;
static IAlloc *ii_frame_allocs = NULL;     // the current frame's temporaries
static void *ii_tmp(size_t n) {
    IAlloc *a = malloc(sizeof *a); a->p = calloc(1, n ? n : 1); a->next = ii_frame_allocs; ii_frame_allocs = a;
    return a->p;
}
static void iv_copy(IVal *dst, const IVal *src, bool owned);   // owned: the tree belongs to an object
static IVal *iv_elems(int n, bool owned) { return owned ? calloc((size_t)(n > 0 ? n : 1), sizeof(IVal)) : ii_tmp(sizeof(IVal) * (size_t)(n > 0 ? n : 1)); }
static void iv_copy(IVal *dst, const IVal *src, bool owned) {
    *dst = *src;
    if ((src->k == IV_AGG || src->k == IV_SUM) && src->e && src->n >= 0) {
        int n = src->k == IV_AGG ? src->n : (int)src->len;
        dst->e = iv_elems(n, owned);
        for (int k = 0; k < n; k++) iv_copy(&dst->e[k], &src->e[k], owned);
    }
}
static void iv_free_owned(IVal *v) {
    if ((v->k == IV_AGG || v->k == IV_SUM) && v->e) {
        int n = v->k == IV_AGG ? v->n : (int)v->len;
        for (int k = 0; k < n; k++) iv_free_owned(&v->e[k]);
        free(v->e); v->e = NULL;
    }
}

// The storage a slot of type `t` starts as: aggregates as their parts, everything else uninitialised.
static void iv_shape(IVal *v, IrType *t, bool owned) {
    memset(v, 0, sizeof *v);
    if (!t) { v->k = IV_UNINIT; return; }
    if (t->kind == IRT_STRUCT && !ir_struct_layout(t).packed) {
        v->k = IV_AGG; v->n = t->n_fields; v->e = iv_elems(t->n_fields, owned);
        for (int k = 0; k < t->n_fields; k++) iv_shape(&v->e[k], t->fields[k], owned);
    } else if (t->kind == IRT_STRUCT) {          // [packed]: abstract fields as well
        v->k = IV_AGG; v->n = t->n_fields; v->e = iv_elems(t->n_fields, owned);
        for (int k = 0; k < t->n_fields; k++) iv_shape(&v->e[k], t->fields[k], owned);
    } else if (t->kind == IRT_ARRAY && t->array_len >= 0) {
        v->k = IV_AGG; v->n = (int32_t)t->array_len; v->e = iv_elems(v->n, owned);
        for (int k = 0; k < v->n; k++) iv_shape(&v->e[k], t->elem, owned);
    } else if (t->kind == IRT_VECTOR) {
        v->k = IV_AGG; v->n = (int32_t)t->array_len; v->e = iv_elems(v->n, owned);
        for (int k = 0; k < v->n; k++) v->e[k].k = IV_UNINIT;
    } else v->k = IV_UNINIT;
}

// ── objects and pointers ────────────────────────────────────────────────────────────────
static IFrame *ii_frame = NULL;
// The seal whose initialisation is running a CALL: its callee may write that binding's storage.
static int32_t ii_open_seal = 0;
static IObj *ii_new_obj(IrType *elem, int64_t count, const char *what) {
    IObj *o = calloc(1, sizeof *o);
    o->live = true; o->what = what;
    o->root.k = IV_AGG; o->root.n = (int32_t)count; o->root.e = iv_elems((int)count, true);
    for (int64_t k = 0; k < count; k++) iv_shape(&o->root.e[k], elem, true);
    if (ii_frame) { o->frame_next = ii_frame->objs; ii_frame->objs = o; }
    return o;
}
static IPtr ip_of(IObj *o, int32_t idx) { IPtr p; memset(&p, 0, sizeof p); p.o = o; p.d = 1; p.p[0] = idx; p.lo = 0; p.hi = -1; return p; }

// The aggregate the pointer's last step indexes, and the cell itself.
static IVal *ip_parent(const IPtr *p, IrInstr *at) {
    if (!p->o) II_PROOF(at, "dereference of a null pointer");
    if (!p->o->live) II_PROOF(at, "use of storage that is no longer live (%s)", p->o->what ? p->o->what : "?");
    IVal *cur = &p->o->root;
    for (int k = 0; k < p->d - 1; k++) {
        if (cur->k != IV_AGG || p->p[k] < 0 || p->p[k] >= cur->n) II_UNSUP(at, "a pointer path that does not fit the value it points into");
        cur = &cur->e[p->p[k]];
    }
    if (cur->k != IV_AGG) II_UNSUP(at, "a pointer into a non-aggregate");
    return cur;
}
static IVal *ip_cell(const IPtr *p, IrInstr *at) {
    IVal *par = ip_parent(p, at);
    int32_t ix = p->p[p->d - 1];
    int32_t hi = p->hi >= 0 ? p->hi : par->n;
    if (ix < p->lo || ix >= hi || ix >= par->n) {
        if (at && at->unchecked) II_UB(at, "access at index %d outside [%d, %d)", ix, p->lo, hi);   // `unsafe`: no proof was claimed
        II_PROOF(at, "access at index %d outside [%d, %d)", ix, p->lo, hi);
    }
    return &par->e[ix];
}
static IPtr iv_ptr(const IVal *v, IrInstr *at) {
    if (v->k == IV_PTR) return v->p;
    if (v->k == IV_SLICE) return v->p;
    if (iv_is_null(v)) { IPtr p; memset(&p, 0, sizeof p); return p; }
    if (v->k == IV_UNINIT) II_PROOF(at, "read of an uninitialised pointer");
    II_UNSUP(at, "a value of kind %d used as a pointer", v->k);
    IPtr p; memset(&p, 0, sizeof p); return p;
}

// ── the frame ───────────────────────────────────────────────────────────────────────────
static IVal *ii_val(IrValue *v, IrInstr *at) {
    if (!v || v->id < 0 || v->id >= ii_frame->nv) II_UNSUP(at, "a value outside the frame");
    return &ii_frame->v[v->id];
}
static void ii_set(IrValue *r, const IVal *x) { if (r && r->id >= 0 && r->id < ii_frame->nv) iv_copy(&ii_frame->v[r->id], x, false); }

// A value LANDING in a place of another integer type: a store into a slot, an argument into its
// parameter, a returned value into the result, a field, a payload. The IR states no cast there
// (the conversion is implicit, and so is C's), but the analysis owes it as a narrowing
// (vra_check_narrow), so it is a discharged proof like any other and is checked here. Without
// this the interpreter carried the out-of-range value on unchecked: `(x as% i32) - 2147483646`
// returned from an i32 function printed -2147483651 here and 2147483645 from the C, and as an
// exit status both were 253, so the false proof showed only where a program happened to print it
// (found with the per-operation soundness harness). Now it stops at the line that owed the
// proof. Inside `unsafe` the conversion is C's: it wraps.
static void ii_land_refined(IVal *v, const IrType *dt, IrInstr *at);   // below
static void ii_land(IVal *v, const IrType *vt, const IrType *dt, IrInstr *at) {
    if (!v || v->k != IV_INT || !vt || !dt || vt->kind != IRT_INT || dt->kind != IRT_INT) return;
    if (vt->bits == dt->bits && vt->is_signed == dt->is_signed) { ii_land_refined(v, dt, at); return; }
    __int128 x = iv_get(v, (IrType *)vt, at);
    if (it_fits(x, dt)) { ii_land_refined(v, dt, at); return; }
    if (at && at->unchecked) { iv_int(v, it_wrap(x, (IrType *)dt)); return; }
    char b[48]; int n = 0, j = 0; unsigned __int128 u = x < 0 ? -(unsigned __int128)x : (unsigned __int128)x;
    char t[48]; do { t[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    if (x < 0) b[j++] = '-';
    while (n) b[j++] = t[--n];
    b[j] = 0;
    II_PROOF(at, "%s lands in a %s%d, which cannot hold it (proven to fit)", b, dt->is_signed ? "i" : "u", dt->bits);
}

// ★ A REFINED SLOT IS PART OF THE OBLIGATION. The analysis proves a value fits `i32 >= 0 and <= 3`
// (vra_check_narrow reads the refined range), and the check above compared only the BITS: it
// returned at once for an i32 landing in a refined i32, so a false proof of a refinement was
// never caught where it was owed, only by whatever it broke later. A narrowing into a refined
// type is never waived, inside `unsafe` either (I.60), so a value outside the refinement here is
// a false proof wherever it lands: a store, a field, a payload, an argument, a result.
static void ii_land_refined(IVal *v, const IrType *dt, IrInstr *at) {
    if (!v || v->k != IV_INT || !dt || dt->kind != IRT_INT || !(dt->has_refine || dt->has_ne)) return;
    __int128 x = iv_get(v, (IrType *)dt, at);
    if (dt->has_refine && (x < (__int128)dt->refine_lo || x > (__int128)dt->refine_hi))
        II_PROOF(at, "%lld lands in a type refined to [%lld, %lld], which does not hold it (proven to fit)",
                 (long long)x, (long long)dt->refine_lo, (long long)dt->refine_hi);
    if (dt->has_ne && x == (__int128)dt->refine_ne)
        II_PROOF(at, "%lld lands in a type refined to != %lld (proven to fit)", (long long)x, (long long)dt->refine_ne);
}

static IrFunc *ii_find(const IrName *n) {
    if (!n) return NULL;
    for (IrFunc *g = ist->mod; g; g = g->next)
        if (g->name && g->name->length == n->length && memcmp(g->name->name, n->name, (size_t)n->length) == 0) return g;
    return NULL;
}
static bool ii_name_is(const IrName *n, const char *s) { size_t l = strlen(s); return n && (size_t)n->length == l && memcmp(n->name, s, l) == 0; }

static void ii_call(IrFunc *f, IVal *args, int nargs, IVal *ret, IrInstr *at);

// ── C's `printf`, over interpreted values ────────────────────────────────────────────────
static void ii_cstring(const IVal *v, char *buf, size_t cap, IrInstr *at) {
    IPtr p = iv_ptr(v, at);
    p.lo = 0; p.hi = -1;      // C reads raw memory to the NUL, past a slice's own end (u8[:0])
    size_t n = 0;
    for (;;) {
        IVal *c = ip_cell(&p, at);
        __int128 ch = iv_get(c, NULL, at);
        if ((ch & 0xff) == 0 || n + 1 >= cap) break;
        buf[n++] = (char)(ch & 0xff);
        p.p[p.d - 1]++;
    }
    buf[n] = 0;
}
static void ii_printf(IVal *args, int nargs, IrValue **avals, IrInstr *at, IVal *ret) {
    char fmt[4096]; ii_cstring(&args[0], fmt, sizeof fmt, at);
    char out[16384]; size_t on = 0; int ai = 1;
    for (const char *s = fmt; *s; s++) {
        if (*s != '%') { if (on + 1 < sizeof out) out[on++] = *s; continue; }
        if (s[1] == '%') { if (on + 1 < sizeof out) out[on++] = '%'; s++; continue; }
        char spec[64]; int sl = 0; spec[sl++] = '%'; s++;
        while (*s && strchr("-+ #0123456789.", *s) && sl < 40) spec[sl++] = *s++;
        while (*s && strchr("hlLqjzt", *s)) s++;                  // length: re-spelled below
        char conv = *s; if (!conv) break;
        if (ai >= nargs) II_UNSUP(at, "printf: fewer arguments than conversions");
        IVal *a = &args[ai]; IrType *t = avals[ai] ? avals[ai]->type : NULL; ai++;
        char piece[4096]; int pn = 0;
        if (strchr("diouxXc", conv)) {
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            __int128 x = a->k == IV_FLT ? (__int128)a->f : iv_get(a, t, at);
            if (conv == 'c') { spec[sl - 3] = 'c'; spec[sl - 2] = 0; pn = snprintf(piece, sizeof piece, spec, (int)x); }
            else if (strchr("di", conv)) pn = snprintf(piece, sizeof piece, spec, (long long)x);
            else pn = snprintf(piece, sizeof piece, spec, (unsigned long long)x);
        } else if (strchr("fFeEgGaA", conv)) {
            spec[sl++] = conv; spec[sl] = 0;
            double x = a->k == IV_FLT ? a->f : (double)iv_get(a, t, at);
            pn = snprintf(piece, sizeof piece, spec, x);
        } else if (conv == 's') {
            spec[sl++] = 's'; spec[sl] = 0;
            char str[4096]; ii_cstring(a, str, sizeof str, at);
            pn = snprintf(piece, sizeof piece, spec, str);
        } else II_UNSUP(at, "printf conversion %%%c", conv);
        for (int k = 0; k < pn && on + 1 < sizeof out; k++) out[on++] = piece[k];
    }
    fwrite(out, 1, on, stdout);
    if (ret) iv_int(ret, (__int128)on);
}

// HEAP storage: `malloc` returns RAW bytes, typed by the first cast to `*T` (n / sizeof T cells).
// Freeing kills the object: a use after free, a double free and a free of an interior pointer
// are each what single ownership (the linearity proof) promises cannot happen.
static int64_t ii_c_size(IrType *t);
static IObj *ii_heap(int64_t bytes, bool zero) {
    IObj *saved = NULL; IFrame *fr = ii_frame; ii_frame = NULL;      // not a frame's: lives until freed
    IObj *o = ii_new_obj(NULL, bytes, "heap storage");
    ii_frame = fr; (void)saved;
    o->heap = true; o->raw = true;
    if (zero) for (int64_t k = 0; k < bytes; k++) iv_int(&o->root.e[k], 0);
    return o;
}
static void ii_free(IVal *a, IrInstr *at) {
    if (iv_is_null(a)) return;
    IPtr p = iv_ptr(a, at);
    if (!p.o || !p.o->heap) II_PROOF(at, "free of storage that was not allocated");
    if (!p.o->live) II_PROOF(at, "double free (single ownership was proven)");
    if (p.d != 1 || p.p[0] != 0) II_PROOF(at, "free of a pointer into the middle of an allocation");
    p.o->live = false; p.o->what = "freed heap storage"; iv_free_owned(&p.o->root); p.o->root.n = 0;
}

// An EXTERN: the few the corpus calls are modelled; anything else is not a verdict.
static void ii_extern(IrFunc *f, IVal *args, int nargs, IrValue **avals, IVal *ret, IrInstr *at) {
    const IrName *n = f->name;
    if (ii_name_is(n, "libc_printf") || ii_name_is(n, "printf")) { ii_printf(args, nargs, avals, at, ret); return; }
    if (ii_name_is(n, "libc_puts") || ii_name_is(n, "puts")) {
        char s[4096]; ii_cstring(&args[0], s, sizeof s, at); fputs(s, stdout); fputc('\n', stdout);
        if (ret) iv_int(ret, 0);
        return;
    }
    if (ii_name_is(n, "putchar") || ii_name_is(n, "libc_putchar")) {
        __int128 c = iv_get(&args[0], avals[0] ? avals[0]->type : NULL, at); fputc((int)(c & 0xff), stdout);
        if (ret) iv_int(ret, c);
        return;
    }
    if (ii_name_is(n, "exit") || ii_name_is(n, "libc_exit")) {
        fflush(stdout); ii_stop((int)(iv_get(&args[0], avals[0] ? avals[0]->type : NULL, at) & 0xff));
    }
    if (ii_name_is(n, "abort") || ii_name_is(n, "libc_abort")) { fflush(stdout); ii_stop(134); }
    bool cal = ii_name_is(n, "libc_calloc") || ii_name_is(n, "calloc");
    if (cal || ii_name_is(n, "libc_malloc") || ii_name_is(n, "malloc")) {
        __int128 cnt = iv_get(&args[0], avals[0] ? avals[0]->type : NULL, at);
        __int128 sz = cal ? iv_get(&args[1], avals[1] ? avals[1]->type : NULL, at) : 1;
        if (cnt * sz > (1 << 26)) II_UNSUP(at, "an allocation of %lld bytes", (long long)(cnt * sz));
        IObj *o = ii_heap((int64_t)(cnt * sz), cal);
        if (ret) { memset(ret, 0, sizeof *ret); ret->k = IV_PTR; ret->p = ip_of(o, 0); }
        return;
    }
    if (ii_name_is(n, "libc_free") || ii_name_is(n, "free")) { ii_free(&args[0], at); if (ret) ret->k = IV_UNIT; return; }
    if (ii_name_is(n, "abs") || ii_name_is(n, "libc_abs")) {
        __int128 x = iv_get(&args[0], avals[0] ? avals[0]->type : NULL, at);
        if (x == -2147483648LL) II_PROOF(at, "abs of INT_MIN (undefined)");
        if (ret) iv_int(ret, x < 0 ? -x : x);
        return;
    }
    II_UNSUP(at, "extern function %.*s", (int)n->length, n->name);
}

// ── one instruction ─────────────────────────────────────────────────────────────────────
static IrType *ii_ty(IrValue *v) { return v ? v->type : NULL; }

// Integer arithmetic on two mathematical values, by the instruction's wrap mode. A CHECK-mode
// result outside its type is a FALSE PROOF: the analysis accepted it as free of overflow.
static __int128 ii_settle(__int128 x, bool ovf, IrType *rt, IrInstr *ins) {
    if (ins->wrap == IR_WRAP_MODULAR) return it_wrap(x, rt);
    // Only an unsigned 64-bit product can overflow 128 bits, and it is positive.
    if (ins->wrap == IR_WRAP_SATURATE) return ovf ? it_max(rt) : it_clamp(x, rt);
    if (ovf || !it_fits(x, rt)) {
        if (ins->unchecked && !it_signed(rt)) return it_wrap_bits(x, ir_int_storage_bits(it_bits(rt)), false);
        if (ins->unchecked) II_UB(ins, "signed arithmetic overflows its type");
        II_PROOF(ins, "arithmetic overflows its type (proven free of overflow)");
    }
    return x;
}

static double ii_fround(double x, IrType *t) { return (t && t->kind == IRT_FLOAT && t->float_bits == 32) ? (double)(float)x : x; }

static void ii_binop_scalar(IrInstr *ins, IVal *a, IVal *b, IrType *ta, IrType *tb, IrType *rt, IVal *r) {
    if (rt && rt->kind == IRT_FLOAT) {
        double x = a->k == IV_FLT ? a->f : (double)iv_get(a, ta, ins);
        double y = b->k == IV_FLT ? b->f : (double)iv_get(b, tb, ins);
        double z;
        switch (ins->op) {
            case IR_ADD: z = x + y; break; case IR_SUB: z = x - y; break; case IR_MUL: z = x * y; break;
            case IR_SDIV: case IR_UDIV: z = x / y; break;
            default: II_UNSUP(ins, "float operation %d", ins->op); z = 0;
        }
        memset(r, 0, sizeof *r); r->k = IV_FLT; r->f = ii_fround(z, rt); return;
    }
    __int128 x = iv_get(a, ta, ins), y = iv_get(b, tb, ins), z = 0; bool ovf = false;
    switch (ins->op) {
        case IR_ADD: ovf = __builtin_add_overflow(x, y, &z); break;
        case IR_SUB: ovf = __builtin_sub_overflow(x, y, &z); break;
        case IR_MUL:
            ovf = __builtin_mul_overflow(x, y, &z);
            if (ovf && ins->wrap == IR_WRAP_MODULAR) { z = (__int128)((unsigned __int128)x * (unsigned __int128)y); ovf = false; }
            break;
        case IR_SDIV: case IR_UDIV:
            // Inside `unsafe` the obligation was waived, so no proof was claimed: a zero divisor
            // there is the program's undefined behaviour, as an out-of-range index is (above).
            if (y == 0 && ins->unchecked) II_UB(ins, "division by zero");
            if (y == 0) II_PROOF(ins, "division by zero (proven nonzero)");
            z = x / y; break;
        case IR_SREM: case IR_UREM:
            if (y == 0 && ins->unchecked) II_UB(ins, "remainder by zero");
            if (y == 0) II_PROOF(ins, "remainder by zero (proven nonzero)");
            z = x % y; break;
        case IR_AND: z = (__int128)((int64_t)x & (int64_t)y); if (!it_signed(rt)) z = it_wrap(z, rt); break;
        case IR_OR:  z = (__int128)((int64_t)x | (int64_t)y); if (!it_signed(rt)) z = it_wrap(z, rt); break;
        case IR_XOR: z = (__int128)((int64_t)x ^ (int64_t)y); if (!it_signed(rt)) z = it_wrap(z, rt); break;
        case IR_SHL: case IR_LSHR: case IR_ASHR: {
            int w = it_bits(ta);
            // A vector's lanes shifted past their width read 0. A scalar amount is an
            // obligation in every wrap mode: `<<%` wraps the bits shifted out, not the amount,
            // and the C `<<` by the width or more is undefined.
            bool lane = ins->result && ins->result->type && ins->result->type->kind == IRT_VECTOR;
            if (y < 0 || y >= w) {
                if (lane) { z = 0; break; }
                if (ins->unchecked) II_UB(ins, "shift by %lld, outside [0, %d)", (long long)y, w);
                II_PROOF(ins, "shift by %lld, outside [0, %d) (proven in range)", (long long)y, w);
            }
            if (ins->op == IR_SHL) {
                // exact for |x| < 2^63; ii_settle wraps `<<%` and checks `<<`, which may not lose
                // a bit for either sign (vra_check_shift)
                z = (__int128)((unsigned __int128)x << (int)y);
            } else if (ins->op == IR_LSHR) {
                z = (__int128)((unsigned __int128)it_wrap_bits(x, w, false) >> (int)y);
            } else z = x >> (int)y;
            break;
        }
        default: II_UNSUP(ins, "integer operation %d", ins->op);
    }
    iv_int(r, ii_settle(z, ovf, rt, ins));
}

// Lane-wise, wrapping: vector arithmetic carries no overflow obligation (spec 7.5).
static void ii_binop(IrInstr *ins, IVal *r) {
    IVal *a = ii_val(ins->operands[0], ins), *b = ii_val(ins->operands[1], ins);
    IrType *ta = ii_ty(ins->operands[0]), *tb = ii_ty(ins->operands[1]), *rt = ii_ty(ins->result);
    if (rt && rt->kind == IRT_VECTOR) {
        int n = (int)rt->array_len; IrType *lt = rt->elem;
        memset(r, 0, sizeof *r); r->k = IV_AGG; r->n = n; r->e = iv_elems(n, false);
        for (int k = 0; k < n; k++) {
            IVal *x = (a->k == IV_AGG) ? &a->e[k] : a, *y = (b->k == IV_AGG) ? &b->e[k] : b;
            IrType *lx = (ta && ta->kind == IRT_VECTOR) ? ta->elem : ta, *ly = (tb && tb->kind == IRT_VECTOR) ? tb->elem : tb;
            IrInstr lane = *ins; lane.wrap = (lt && lt->kind == IRT_INT) ? IR_WRAP_MODULAR : ins->wrap;
            ii_binop_scalar(&lane, x, y, lx, ly, lt, &r->e[k]);
        }
        return;
    }
    ii_binop_scalar(ins, a, b, ta, tb, rt, r);
}

static bool ii_cmp_scalar(IrInstr *ins, IVal *a, IVal *b, IrType *ta, IrType *tb) {
    IrCmp c = ins->aux.cmp;
    if (a->k == IV_FLT || b->k == IV_FLT) {
        double x = a->k == IV_FLT ? a->f : (double)iv_get(a, ta, ins), y = b->k == IV_FLT ? b->f : (double)iv_get(b, tb, ins);
        switch (c) { case IR_CMP_EQ: return x == y; case IR_CMP_NE: return x != y;
            case IR_CMP_SLT: case IR_CMP_ULT: return x < y;  case IR_CMP_SLE: case IR_CMP_ULE: return x <= y;
            case IR_CMP_SGT: case IR_CMP_UGT: return x > y;  default: return x >= y; }
    }
    if (a->k == IV_PTR || b->k == IV_PTR || a->k == IV_SLICE || b->k == IV_SLICE) {
        bool an = a->k != IV_PTR, bn = b->k != IV_PTR;        // an integer here is the null pointer
        bool eq = (an && bn) || (!an && !bn && a->p.o == b->p.o && a->p.d == b->p.d &&
                                 memcmp(a->p.p, b->p.p, sizeof(int32_t) * (size_t)a->p.d) == 0);
        if (c == IR_CMP_EQ) return eq;
        if (c == IR_CMP_NE) return !eq;
        II_UNSUP(ins, "an ordering comparison of pointers");
    }
    __int128 x = iv_get(a, ta, ins), y = iv_get(b, tb, ins);
    if (c >= IR_CMP_ULT) { x = (__int128)(uint64_t)x; y = (__int128)(uint64_t)y; }   // as unsigned 64-bit
    switch (c) {
        case IR_CMP_EQ: return x == y; case IR_CMP_NE: return x != y;
        case IR_CMP_SLT: case IR_CMP_ULT: return x < y; case IR_CMP_SLE: case IR_CMP_ULE: return x <= y;
        case IR_CMP_SGT: case IR_CMP_UGT: return x > y; default: return x >= y;
    }
}

static void ii_cast(IrInstr *ins, IVal *r) {
    IVal *a = ii_val(ins->operands[0], ins);
    IrType *st = ii_ty(ins->operands[0]), *dt = ii_ty(ins->result);
    if (!dt) { *r = *a; return; }
    if (dt->kind == IRT_VECTOR && a->k == IV_AGG) {          // a lane-wise reinterpretation
        memset(r, 0, sizeof *r); r->k = IV_AGG; r->n = a->n; r->e = iv_elems(a->n, false);
        for (int k = 0; k < a->n; k++) {
            if (a->e[k].k == IV_FLT || (dt->elem && dt->elem->kind == IRT_FLOAT)) { r->e[k] = a->e[k]; continue; }
            iv_int(&r->e[k], it_wrap(iv_get(&a->e[k], st && st->kind == IRT_VECTOR ? st->elem : NULL, ins), dt->elem));
        }
        return;
    }
    if (dt->kind == IRT_FLOAT) {
        double x = a->k == IV_FLT ? a->f
                 : it_signed(st) ? (dt->float_bits == 32 ? (double)(float)(int64_t)iv_get(a, st, ins) : (double)(int64_t)iv_get(a, st, ins))
                 : (dt->float_bits == 32 ? (double)(float)(uint64_t)iv_get(a, st, ins) : (double)(uint64_t)iv_get(a, st, ins));
        memset(r, 0, sizeof *r); r->k = IV_FLT; r->f = ii_fround(x, dt); return;
    }
    if ((dt->kind == IRT_INT || dt->kind == IRT_BOOL) && a->k == IV_FLT) {
        double x = a->f;
        if (!(x > (double)it_min(dt) - 1.0 && x < (double)it_max(dt) + 1.0)) II_PROOF(ins, "a float out of the integer type's range");
        iv_int(r, (__int128)x); return;
    }
    if (dt->kind == IRT_BOOL && (a->k == IV_INT || a->k == IV_FLT)) {     // C's _Bool: nonzero is 1
        iv_int(r, a->k == IV_FLT ? a->f != 0.0 : iv_get(a, st, ins) != 0); return;
    }
    if ((dt->kind == IRT_INT || dt->kind == IRT_BOOL) && a->k == IV_INT) {
        __int128 x = iv_get(a, st, ins);
        if (ins->wrap == IR_WRAP_MODULAR) { iv_int(r, it_wrap(x, dt)); return; }
        if (ins->wrap == IR_WRAP_SATURATE) { iv_int(r, it_clamp(x, dt)); return; }
        if (!it_fits(x, dt)) {
            if (ins->unchecked) { iv_int(r, it_wrap_bits(x, ir_int_storage_bits(it_bits(dt)), it_signed(dt))); return; }
            II_PROOF(ins, "conversion of %lld to a type that cannot hold it (proven to fit)", (long long)x);
        }
        iv_int(r, x); return;
    }
    if (dt->kind == IRT_PTR || dt->kind == IRT_FUNC) {
        if (a->k == IV_PTR && a->p.o && a->p.o->raw && dt->kind == IRT_PTR && dt->elem &&
            dt->elem->kind != IRT_UNIT && !(dt->elem->kind == IRT_INT && dt->elem->bits <= 8) &&
            ii_c_size(dt->elem) > 0) {                          // `*void` has no size: it passes through
            IObj *o = a->p.o; int64_t es = ii_c_size(dt->elem), bytes = o->root.n;
            if (bytes % es != 0 || a->p.d != 1 || a->p.p[0] != 0) II_UNSUP(ins, "raw heap bytes viewed as a type that does not tile them");
            iv_free_owned(&o->root);
            IFrame *fr = ii_frame; ii_frame = NULL;
            IVal nr; memset(&nr, 0, sizeof nr); nr.k = IV_AGG; nr.n = (int32_t)(bytes / es); nr.e = iv_elems(nr.n, true);
            for (int32_t k = 0; k < nr.n; k++) iv_shape(&nr.e[k], dt->elem, true);
            ii_frame = fr; o->root = nr; o->raw = false;
        }
        if (a->k == IV_PTR || a->k == IV_FUNC || iv_is_null(a)) { *r = *a; return; }
        II_UNSUP(ins, "an integer converted to a pointer");
    }
    if ((dt->kind == IRT_INT) && (a->k == IV_PTR)) II_UNSUP(ins, "a pointer converted to an integer");
    *r = *a;   // same representation (a struct to itself, a sum to itself)
}

// The C layout model, for @sizeof/@alignof of a type whose size C decides (an ordinary struct):
// natural alignment, fields in the order the layout chooses. A disagreement with the emitted C
// is a layout bug in one of the two, which is what the differential is for.
static int64_t ii_c_align(IrType *t);
static int64_t ii_c_size(IrType *t);
// A struct's fields at natural alignment, in storage order `ord` (0 past the reorder limit).
static int64_t ii_c_fields_size(IrType *t, const int *ord) {
    if (t->n_fields > IR_REORDER_MAX_FIELDS) return 0;
    int64_t off = 0, al = 1;
    for (int q = 0; q < t->n_fields; q++) {
        IrType *ft = t->fields[ord[q]]; int64_t a = ii_c_align(ft), z = ii_c_size(ft);
        if (a > al) al = a;
        off = (off + a - 1) / a * a + z;
    }
    return (off + al - 1) / al * al;
}
static int64_t ii_c_size(IrType *t) {
    int64_t s = ir_fixed_size(t);
    if (s > 0) return s;
    if (!t) return 0;
    switch (t->kind) {
        case IRT_BOOL: return 1;
        case IRT_FLOAT: return t->float_bits == 32 ? 4 : 8;
        case IRT_PTR: case IRT_FUNC: return 8;
        case IRT_SLICE: return 16;
        case IRT_ARRAY: return t->array_len * ii_c_size(t->elem);
        case IRT_STRUCT: {
            int ord[IR_REORDER_MAX_FIELDS + 1];
            if (t->n_fields > IR_REORDER_MAX_FIELDS) return 0;
            ir_struct_storage_order(t, ir_layout_iface(t), ord);
            return ii_c_fields_size(t, ord);
        }
        case IRT_SUM: {
            IrLayout L = ir_layout_of(t);
            if (L.packed && L.backing) return L.backing->kind == IRT_BOOL ? 1 : ii_c_size(L.backing);
            int64_t tb = ir_sum_tag_bits(t) / 8, al = tb, pay = 0;
            for (int k = 0; k < t->n_fields; k++) if (t->fields[k]) {
                // the payload in the SUM's order: its own name is its variant's
                int ord[IR_REORDER_MAX_FIELDS + 1];
                if (t->fields[k]->n_fields > IR_REORDER_MAX_FIELDS) return 0;
                ir_sum_payload_order(t, k, ir_layout_iface(t), ord);
                int64_t a = ii_c_align(t->fields[k]), z = ii_c_fields_size(t->fields[k], ord);
                if (a > al) al = a;
                if (z > pay) pay = z;
            }
            int64_t off = (tb + al - 1) / al * al + pay;
            return (off + al - 1) / al * al;
        }
        default: return 0;
    }
}
static int64_t ii_c_align(IrType *t) {
    if (!t) return 1;
    switch (t->kind) {
        case IRT_ARRAY: return ii_c_align(t->elem);
        case IRT_STRUCT: {
            if (ir_struct_layout(t).packed) return ir_struct_layout(t).container_bits / 8;
            int64_t al = 1; for (int k = 0; k < t->n_fields; k++) { int64_t a = ii_c_align(t->fields[k]); if (a > al) al = a; }
            return al;
        }
        default: { int64_t s = ii_c_size(t); return t->kind == IRT_SUM ? ir_order_align(t) : (s > 0 && s <= 16 ? s : 8); }
    }
}

static void ii_exec(IrInstr *ins) {
    IVal r; memset(&r, 0, sizeof r);
    IrType *rt = ii_ty(ins->result);
    switch (ins->op) {
        case IR_CONST:
            if (rt && rt->kind == IRT_FLOAT) { r.k = IV_FLT; r.f = ii_fround(ins->aux.fimm, rt); }
            else iv_int(&r, (__int128)ins->aux.imm);
            break;
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM:
        case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_LSHR: case IR_ASHR:
            ii_binop(ins, &r); break;
        case IR_NEG: {
            IVal *a = ii_val(ins->operands[0], ins); IrType *ta = ii_ty(ins->operands[0]);
            if (rt && rt->kind == IRT_VECTOR) {
                r.k = IV_AGG; r.n = a->n; r.e = iv_elems(a->n, false);
                for (int k = 0; k < a->n; k++) {
                    if (a->e[k].k == IV_FLT) { r.e[k] = a->e[k]; r.e[k].f = -a->e[k].f; }
                    else iv_int(&r.e[k], it_wrap(-iv_get(&a->e[k], ta->elem, ins), rt->elem));
                }
            } else if (a->k == IV_FLT) { r.k = IV_FLT; r.f = -a->f; }
            else iv_int(&r, ii_settle(-iv_get(a, ta, ins), false, rt, ins));
            break;
        }
        case IR_BNOT: {
            IVal *a = ii_val(ins->operands[0], ins); IrType *ta = ii_ty(ins->operands[0]);
            if (rt && rt->kind == IRT_VECTOR) {
                r.k = IV_AGG; r.n = a->n; r.e = iv_elems(a->n, false);
                for (int k = 0; k < a->n; k++) iv_int(&r.e[k], it_wrap(~iv_get(&a->e[k], ta->elem, ins), rt->elem));
            } else iv_int(&r, it_wrap(~iv_get(a, ta, ins), rt));
            break;
        }
        case IR_CTZ: case IR_CLZ: case IR_POPCOUNT: {
            IVal *a = ii_val(ins->operands[0], ins); IrType *ta = ii_ty(ins->operands[0]);
            uint64_t x = (uint64_t)it_wrap_bits(iv_get(a, ta, ins), it_bits(ta), false); int w = it_bits(ta), n = 0;
            if (ins->op == IR_POPCOUNT) n = __builtin_popcountll(x);
            else if (x == 0) II_PROOF(ins, "%s of zero (undefined; proven nonzero)", ins->op == IR_CTZ ? "@ctz" : "@clz");
            else if (ins->op == IR_CTZ) n = __builtin_ctzll(x);
            else n = __builtin_clzll(x) - (64 - w);
            iv_int(&r, n);
            break;
        }
        case IR_SIZEOF: case IR_ALIGNOF: {
            int64_t s = ins->op == IR_SIZEOF ? ii_c_size(ins->aux.alloca_ty) : ii_c_align(ins->aux.alloca_ty);
            if (s <= 0) II_UNSUP(ins, "the size of this type");
            iv_int(&r, s);
            break;
        }
        case IR_ICMP: {
            IVal *a = ii_val(ins->operands[0], ins), *b = ii_val(ins->operands[1], ins);
            IrType *ta = ii_ty(ins->operands[0]), *tb = ii_ty(ins->operands[1]);
            if (rt && rt->kind == IRT_VECTOR) {
                int n = (int)rt->array_len; r.k = IV_AGG; r.n = n; r.e = iv_elems(n, false);
                for (int k = 0; k < n; k++) {
                    IVal *x = a->k == IV_AGG ? &a->e[k] : a, *y = b->k == IV_AGG ? &b->e[k] : b;
                    IrType *lx = (ta && ta->kind == IRT_VECTOR) ? ta->elem : ta, *ly = (tb && tb->kind == IRT_VECTOR) ? tb->elem : tb;
                    iv_int(&r.e[k], ii_cmp_scalar(ins, x, y, lx, ly) ? -1 : 0);   // a mask lane: every bit
                }
            } else iv_int(&r, ii_cmp_scalar(ins, a, b, ta, tb) ? 1 : 0);
            break;
        }
        case IR_CAST: ii_cast(ins, &r); break;
        case IR_ALLOCA: {
            if (ins->data) {            // a module constant table: ONE read-only object, ever
                IrData *d = ins->data;
                IObj *o = (IObj *)d->interp_obj;
                if (!o) {
                    o = ii_new_obj(d->type->elem, d->n, "a module constant table");
                    if (ii_frame) { ii_frame->objs = o->frame_next; o->frame_next = NULL; }   // static: never dies
                    for (int k = 0; k < d->n; k++) iv_int(&o->root.e[k], (uint64_t)d->vals[k]);
                    o->ro = true;
                    d->interp_obj = o;
                }
                r.k = IV_PTR; r.p = ip_of(o, 0);
                break;
            }
            int64_t count = 1;
            IrType *at = ins->aux.alloca_ty;
            if (ins->n_operands >= 1) count = (int64_t)iv_get(ii_val(ins->operands[0], ins), ii_ty(ins->operands[0]), ins);
            if (at && at->kind == IRT_ARRAY && ins->n_operands == 0) {
                IObj *o = ii_new_obj(at->elem, at->array_len, "an array in a frame");
                o->seal = ins->seal;
                r.k = IV_PTR; r.p = ip_of(o, 0);
            } else {
                IObj *o = ii_new_obj(at, count, "a local in a frame");
                o->seal = ins->seal;
                r.k = IV_PTR; r.p = ip_of(o, 0);
            }
            break;
        }
        case IR_LOAD: {
            IPtr p = iv_ptr(ii_val(ins->operands[0], ins), ins);
            IrType *pt = ii_ty(ins->operands[0]);
            if (rt && rt->kind == IRT_VECTOR && pt && pt->elem && pt->elem->kind != IRT_VECTOR) {   // a wide load
                int n = (int)rt->array_len; r.k = IV_AGG; r.n = n; r.e = iv_elems(n, false);
                for (int k = 0; k < n; k++) { IPtr q = p; q.p[q.d - 1] += k; iv_copy(&r.e[k], ip_cell(&q, ins), false); }
                break;
            }
            IVal *c = ip_cell(&p, ins);
            if (c->k == IV_UNINIT && rt && rt->kind != IRT_STRUCT && rt->kind != IRT_ARRAY && rt->kind != IRT_VECTOR) {
                // Heap storage is reached only through raw pointers, inside `unsafe`, where the
                // initialisation analysis promises nothing: reading it unwritten is the program's UB.
                if (p.o && p.o->heap) II_UB(ins, "read of heap memory never written (malloc does not initialise)");
                II_PROOF(ins, "read of an uninitialised value (definite initialisation)");
            }
            iv_copy(&r, c, false);
            // A plain enumeration's storage holds its ordinal, and only Lain's own values reach it,
            // except a raw write in `unsafe` (`*(&k as *var u8) = 3`). A value that is none of the
            // variants is undefined: a sum that stores its empty variants in the enum's spare
            // values (layout.h) reads it as one of THEM, `Some(k)` as `None`.
            if (rt && rt->kind == IRT_SUM && r.k == IV_INT && ir_layout_of(rt).all_empty) {
                if (r.i >= (uint64_t)rt->n_fields)
                    II_UB(ins, "an enumeration holds %llu, which is none of its %d variants (written as raw bytes)",
                          (unsigned long long)r.i, rt->n_fields);
                int32_t ord = (int32_t)r.i;
                memset(&r, 0, sizeof r); r.k = IV_SUM; r.n = ord;
            }
            // A tagged sum's storage written as raw bytes, which reach its tag first: a tag that
            // is none of the variants is undefined, as an enumeration's is (the C's `case` takes
            // no arm). A valid one over payload bytes the model does not hold is not modelled.
            if (rt && rt->kind == IRT_SUM && r.k == IV_INT && !ir_layout_of(rt).packed) {
                if (r.i >= (uint64_t)rt->n_fields)
                    II_UB(ins, "a tagged sum's tag holds %llu, which is none of its %d variants (written as raw bytes)",
                          (unsigned long long)r.i, rt->n_fields);
                II_UNSUP(ins, "a tagged sum written as raw bytes");
            }
            break;
        }
        case IR_STORE: {
            IPtr p = iv_ptr(ii_val(ins->operands[0], ins), ins);
            IVal *v = ii_val(ins->operands[1], ins);
            IrType *pt = ii_ty(ins->operands[0]), *vt = ii_ty(ins->operands[1]);
            if (p.o && p.o->ro) II_PROOF(ins, "a write to read-only storage");
            // An immutable binding's storage, written from outside its initialisation: the
            // check E009 makes statically (borrow.h), held to account here.
            if (p.o && p.o->seal && p.o->seal != ins->seal && p.o->seal != ii_open_seal) {
                if (ins->unchecked) II_UB(ins, "a write to immutable storage");
                II_PROOF(ins, "a write to immutable storage (a binding without `var`, or a module constant) outside its initialisation");
            }
            if (vt && vt->kind == IRT_VECTOR && pt && pt->elem && pt->elem->kind != IRT_VECTOR) {   // a wide store
                for (int k = 0; k < v->n; k++) { IPtr q = p; q.p[q.d - 1] += k; IVal *c = ip_cell(&q, ins); iv_free_owned(c); iv_copy(c, &v->e[k], true); }
                return;
            }
            IVal *c = ip_cell(&p, ins);
            // An ARRAY value is its decayed base: storing one copies the elements it points at.
            if (vt && vt->kind == IRT_ARRAY && v->k == IV_PTR && c->k == IV_AGG) {
                IPtr s = v->p;
                for (int k = 0; k < c->n; k++) { IPtr q = s; q.p[q.d - 1] += k; IVal *src = ip_cell(&q, ins); iv_free_owned(&c->e[k]); iv_copy(&c->e[k], src, true); }
                return;
            }
            iv_free_owned(c); iv_copy(c, v, true);
            ii_land(c, vt, pt && pt->kind == IRT_PTR ? pt->elem : NULL, ins);
            return;
        }
        case IR_FIELD_PTR: {
            IPtr p = iv_ptr(ii_val(ins->operands[0], ins), ins);
            if (p.d + 2 > IP_MAX) II_UNSUP(ins, "a pointer path deeper than %d", IP_MAX);
            (void)ip_cell(&p, ins);                               // the struct itself must be there
            p.p[p.d++] = ins->aux.field_idx; p.lo = 0; p.hi = -1;
            if (rt && rt->kind == IRT_ARRAY) p.p[p.d++] = 0;      // an array field decays to its base
            r.k = IV_PTR; r.p = p;
            break;
        }
        case IR_ELEM_PTR: {
            IVal *b = ii_val(ins->operands[0], ins);
            __int128 ix = iv_get(ii_val(ins->operands[1], ins), ii_ty(ins->operands[1]), ins);
            IrType *bt = ii_ty(ins->operands[0]);
            IPtr p;
            if (b->k == IV_AGG && bt && bt->kind == IRT_VECTOR) {   // a lane of a vector VALUE
                IObj *o = ii_new_obj(NULL, 1, "a vector temporary");
                iv_free_owned(&o->root.e[0]); iv_copy(&o->root.e[0], b, true);
                p = ip_of(o, 0); p.p[p.d++] = 0; p.lo = 0; p.hi = -1;
            } else p = iv_ptr(b, ins);
            if (bt && bt->kind == IRT_PTR && bt->elem && bt->elem->kind == IRT_VECTOR) {   // a lane through a pointer
                if (p.d + 1 > IP_MAX) II_UNSUP(ins, "a pointer path too deep");
                p.p[p.d++] = 0; p.lo = 0; p.hi = -1;
            }
            int64_t ni = (int64_t)p.p[p.d - 1] + (int64_t)ix;
            IVal *par = ip_parent(&p, ins);
            int32_t hi = p.hi >= 0 ? p.hi : par->n;
            // ONE PAST THE END is a pointer, not an access (C 6.5.6p8): the start of an empty
            // subslice at the end, `s[s.len..s.len]`, is exactly that, and was a "PROOF FAILED"
            // here although the program is defined and the C returns 0. A load or store through it
            // is still refused, by ip_cell, at the access.
            if (ni == hi && !(ins->result && ins->result->type && ins->result->type->kind == IRT_VECTOR)) {
                p.p[p.d - 1] = (int32_t)ni;
                r.k = IV_PTR; r.p = p;
                break;
            }
            if (ni < p.lo || ni >= hi) {
                if (ins->unchecked) II_UB(ins, "index %lld outside [0, %d)", (long long)(ni - p.lo), hi - p.lo);
                II_PROOF(ins, "index %lld outside [0, %d) (proven in bounds)", (long long)(ni - p.lo), hi - p.lo);
            }
            p.p[p.d - 1] = (int32_t)ni;
            r.k = IV_PTR; r.p = p;
            break;
        }
        case IR_SLICE_LEN: { IVal *s = ii_val(ins->operands[0], ins); if (s->k != IV_SLICE) II_UNSUP(ins, "len of a non-slice"); iv_int(&r, s->len); break; }
        case IR_SLICE_DATA: { IVal *s = ii_val(ins->operands[0], ins); if (s->k != IV_SLICE) II_UNSUP(ins, "data of a non-slice"); r.k = IV_PTR; r.p = s->p; break; }
        case IR_MAKE_SLICE: {
            IVal *d = ii_val(ins->operands[0], ins);
            __int128 n = iv_get(ii_val(ins->operands[1], ins), ii_ty(ins->operands[1]), ins);
            r.k = IV_SLICE; r.len = (int64_t)n;
            if (iv_is_null(d)) { memset(&r.p, 0, sizeof r.p); if (n != 0) II_PROOF(ins, "a slice of %lld elements over a null pointer", (long long)n); break; }
            r.p = iv_ptr(d, ins);
            int32_t base = r.p.p[r.p.d - 1];
            IVal *par = ip_parent(&r.p, ins);
            int32_t hi = r.p.hi >= 0 ? r.p.hi : par->n;
            if (n < 0 || base + n > hi) II_PROOF(ins, "a slice of %lld elements from index %d of %d", (long long)n, base, hi);
            r.p.lo = base; r.p.hi = (int32_t)(base + n);
            break;
        }
        case IR_SEQ_EQ: {
            IVal *a = ii_val(ins->operands[0], ins), *b = ii_val(ins->operands[1], ins);
            bool eq = a->len == b->len;
            for (int64_t k = 0; eq && k < a->len; k++) {
                IPtr pa = a->p, pb = b->p; pa.p[pa.d - 1] += (int32_t)k; pb.p[pb.d - 1] += (int32_t)k;
                IVal *x = ip_cell(&pa, ins), *y = ip_cell(&pb, ins);
                eq = x->k == IV_INT && y->k == IV_INT && x->i == y->i;
            }
            iv_int(&r, eq);
            break;
        }
        case IR_STR_CONST: {
            IObj *o = ii_new_obj(NULL, ins->aux.str.len + 1, "a string literal");
            if (ii_frame) { ii_frame->objs = o->frame_next; o->frame_next = NULL; }   // static: never dies
            for (int k = 0; k <= ins->aux.str.len; k++) iv_int(&o->root.e[k], k < ins->aux.str.len ? (unsigned char)ins->aux.str.bytes[k] : 0);
            o->ro = true;
            r.k = IV_PTR; r.p = ip_of(o, 0);
            break;
        }
        case IR_STRUCT_NEW: {
            int n = ins->n_operands;
            r.k = IV_AGG; r.n = n; r.e = iv_elems(n, false);
            for (int k = 0; k < n; k++) {
                IVal *v = ii_val(ins->operands[k], ins);
                IrType *ft = (rt && rt->kind == IRT_STRUCT && k < rt->n_fields) ? rt->fields[k] : NULL;
                if (ft && ft->kind == IRT_ARRAY && v->k == IV_PTR) {        // copy the array it points at
                    IVal *f = &r.e[k]; f->k = IV_AGG; f->n = (int32_t)ft->array_len; f->e = iv_elems(f->n, false);
                    for (int j = 0; j < f->n; j++) { IPtr q = v->p; q.p[q.d - 1] += j; iv_copy(&f->e[j], ip_cell(&q, ins), false); }
                } else { iv_copy(&r.e[k], v, false); ii_land(&r.e[k], ii_ty(ins->operands[k]), ft, ins); }
            }
            break;
        }
        case IR_SUM_NEW: {
            int n = ins->n_operands;
            r.k = IV_SUM; r.n = ins->aux.sum.variant; r.len = n; r.e = iv_elems(n, false);
            const IrType *pl = (rt && rt->kind == IRT_SUM && r.n >= 0 && r.n < rt->n_fields) ? rt->fields[r.n] : NULL;
            for (int k = 0; k < n; k++) {
                IVal *v = ii_val(ins->operands[k], ins);
                const IrType *ft = !pl ? NULL : pl->kind == IRT_STRUCT ? (k < pl->n_fields ? pl->fields[k] : NULL) : (k == 0 ? pl : NULL);
                if (ft && ft->kind == IRT_ARRAY && v->k == IV_PTR) {        // the payload's own copy,
                    IVal *f = &r.e[k]; f->k = IV_AGG; f->n = (int32_t)ft->array_len;   // as IR_STRUCT_NEW
                    f->e = iv_elems(f->n, false);
                    for (int j = 0; j < f->n; j++) { IPtr q = v->p; q.p[q.d - 1] += j; iv_copy(&f->e[j], ip_cell(&q, ins), false); }
                    continue;
                }
                iv_copy(&r.e[k], v, false);
                ii_land(&r.e[k], ii_ty(ins->operands[k]), ft, ins);
            }
            break;
        }
        case IR_SUM_TAG: {
            IVal *s = ii_val(ins->operands[0], ins);
            if (s->k == IV_UNINIT) II_PROOF(ins, "the tag of an uninitialised sum");
            IrType *st = ii_ty(ins->operands[0]);    // raw bytes, however they reached the tag (IR_LOAD)
            if (s->k == IV_INT && st && st->kind == IRT_SUM && s->i >= (uint64_t)st->n_fields)
                II_UB(ins, "a tagged sum's tag holds %llu, which is none of its %d variants (written as raw bytes)",
                      (unsigned long long)s->i, st->n_fields);
            if (s->k != IV_SUM) II_UNSUP(ins, "the tag of a non-sum value");
            iv_int(&r, s->n);
            break;
        }
        case IR_SUM_PAYLOAD: {
            IVal *s = ii_val(ins->operands[0], ins);
            if (s->k != IV_SUM) II_UNSUP(ins, "the payload of a non-sum value");
            if (s->n != ins->aux.sum.variant) II_PROOF(ins, "payload of variant %d read from a value of variant %d", ins->aux.sum.variant, s->n);
            if (ins->aux.sum.field < 0 || ins->aux.sum.field >= s->len) II_UNSUP(ins, "a payload field out of range");
            IVal *fv = &s->e[ins->aux.sum.field];
            if (rt && rt->kind == IRT_ARRAY && fv->k == IV_AGG) {
                // An array value is its decayed base: the C reads `v.data.V.xs` out of the sum's
                // local copy, so the base of a copy that lives as long as this frame.
                IObj *o = ii_new_obj(rt->elem, rt->array_len, "an array read from a sum's payload");
                for (int j = 0; j < fv->n && j < o->root.n; j++) { iv_free_owned(&o->root.e[j]); iv_copy(&o->root.e[j], &fv->e[j], true); }
                r.k = IV_PTR; r.p = ip_of(o, 0);
                break;
            }
            iv_copy(&r, fv, false);
            break;
        }
        case IR_FUNC_REF: r.k = IV_FUNC; r.fn = ins->aux.callee; break;
        case IR_VEC_MOVEMASK: {
            IVal *v = ii_val(ins->operands[0], ins); IrType *vt = ii_ty(ins->operands[0]);
            uint64_t m = 0;
            for (int k = 0; k < v->n; k++) { uint64_t lane = (uint64_t)it_wrap_bits(iv_get(&v->e[k], vt->elem, ins), 8, false); if (lane & 0x80) m |= 1ULL << k; }
            iv_int(&r, (__int128)m);
            break;
        }
        case IR_VEC_SHUFFLE: {
            IVal *t = ii_val(ins->operands[0], ins), *x = ii_val(ins->operands[1], ins);
            IrType *xt = ii_ty(ins->operands[1]);
            int n = (int)rt->array_len; r.k = IV_AGG; r.n = n; r.e = iv_elems(n, false);
            for (int k = 0; k < n; k++) {
                __int128 j = iv_get(&x->e[k], xt->elem, ins);
                if (j >= 0 && j < n) iv_copy(&r.e[k], &t->e[j], false); else iv_int(&r.e[k], 0);
            }
            break;
        }
        case IR_CALL: {
            int a0 = ins->aux.callee ? 0 : 1;
            IrFunc *g = ins->aux.callee ? ii_find(ins->aux.callee) : NULL;
            if (!ins->aux.callee) {
                IVal *fv = ii_val(ins->operands[0], ins);
                if (fv->k != IV_FUNC) II_PROOF(ins, "an indirect call through a value that is not a function");
                g = ii_find(fv->fn);
            }
            int na = ins->n_operands - a0;
            IVal *args = ii_tmp(sizeof(IVal) * (size_t)(na > 0 ? na : 1));
            for (int k = 0; k < na; k++) iv_copy(&args[k], ii_val(ins->operands[a0 + k], ins), false);
            if (g) {                                               // each argument lands in its parameter
                int k = 0;
                for (IrParam *pp = g->params; pp && k < na; pp = pp->next, k++)
                    ii_land(&args[k], ii_ty(ins->operands[a0 + k]), pp->value ? pp->value->type : NULL, ins);
            }
            if (!g) {
                if (ins->aux.callee && ii_name_is(ins->aux.callee, "panic")) { fflush(stdout); ii_stop(134); }
                II_UNSUP(ins, "a call to %.*s, which the module does not define", ins->aux.callee ? (int)ins->aux.callee->length : 1, ins->aux.callee ? ins->aux.callee->name : "?");
            }
            IVal ret; memset(&ret, 0, sizeof ret); ret.k = IV_UNIT;
            int32_t open_saved = ii_open_seal;
            if (ins->seal) ii_open_seal = ins->seal;              // part of a binding's initialiser
            if (g->is_extern) ii_extern(g, args, na, ins->operands + a0, &ret, ins);
            else ii_call(g, args, na, &ret, ins);
            ii_open_seal = open_saved;
            if (ins->result) ii_set(ins->result, &ret);
            return;
        }
        case IR_ASSERT: {
            IVal *c = ii_val(ins->operands[0], ins);
            if (iv_get(c, NULL, ins) == 0) II_PROOF(ins, "an obligation the analysis discharged does not hold (assert, code %lld)", (long long)ins->aux.imm);
            return;
        }
        case IR_ASSUME: {
            if (ins->n_operands < 1) return;
            IVal *c = ii_val(ins->operands[0], ins);
            if (c->k == IV_INT && c->i == 0) {
                if (ins->aux.imm == 1) ii_fail(99, "TRUSTED ASSUMPTION VIOLATED", ins, "an `assume` or a contract believed without proof is false here");
                II_PROOF(ins, "an assumption the compiler established is false here");
            }
            return;
        }
        case IR_SHAPE: case IR_INIT: case IR_BORROW: case IR_BORROW_END: case IR_CONSUME: return;
        case IR_OPAQUE: II_UNSUP(ins, "an unmodelled construct (%s)", ins->aux.opaque.why ? ins->aux.opaque.why : "?");
        default: II_UNSUP(ins, "instruction %d", ins->op);
    }
    if (ins->result) ii_set(ins->result, &r);
}

// ── a call ──────────────────────────────────────────────────────────────────────────────
// Called at every block entry with the frame's values, when set: the analysis's invariant at
// that block must contain them (src/analysis/containment.h, `lain --interpret --check-invariants`).
// A hook rather than a call, so the semantics does not depend on the analysis it checks.
static void (*ii_on_block)(IrFunc *f, IrBlock *b, IVal *v, int nv) = NULL;
static void ii_call(IrFunc *f, IVal *args, int nargs, IVal *ret, IrInstr *at) {
    IFrame fr; memset(&fr, 0, sizeof fr);
    fr.f = f; fr.nv = f->next_value_id > 0 ? f->next_value_id : 1; fr.up = ii_frame;
    IAlloc *saved_allocs = ii_frame_allocs; ii_frame_allocs = NULL;
    fr.v = ii_tmp(sizeof(IVal) * (size_t)fr.nv);
    IFrame *caller = ii_frame; ii_frame = &fr;
    IrFunc *caller_f = ii_cur_f; ii_cur_f = f;
    int k = 0;
    for (IrParam *p = f->params; p; p = p->next, k++) {
        if (k >= nargs) II_UNSUP(at, "a call with fewer arguments than parameters");
        if (p->value) iv_copy(&fr.v[p->value->id], &args[k], false);
    }
    IrBlock *b = f->entry, *prev = NULL;
    for (;;) {
        if (!b) II_UNSUP(at, "a function without an entry block");
        for (IrInstr *phi = b->phis; phi; phi = phi->next)
            for (IrPhiArg *pa = phi->phi_args; pa; pa = pa->next)
                if (pa->pred == prev) { ii_set(phi->result, ii_val(pa->value, phi)); break; }
        if (ii_on_block) ii_on_block(f, b, fr.v, fr.nv);
        for (IrInstr *i = b->instrs; i; i = i->next) {
            if (++ist->steps > ist->budget) ii_fail(97, "STEP BUDGET EXHAUSTED", i, "%lld steps", ist->budget);
            ii_exec(i);
        }
        prev = b;
        IrTerm *t = &b->term;
        if (t->kind == IR_TERM_BR) { b = t->a; continue; }
        if (t->kind == IR_TERM_BR_COND) {
            __int128 c = iv_get(ii_val(t->cond, NULL), NULL, NULL);
            b = c ? t->a : t->b; continue;
        }
        if (t->kind == IR_TERM_SWITCH) {
            __int128 c = iv_get(ii_val(t->cond, NULL), ii_ty(t->cond), NULL);
            IrBlock *to = t->a;
            for (IrSwitchCase *sc = t->cases; sc; sc = sc->next) if (sc->key == (int64_t)c) { to = sc->target; break; }
            b = to; continue;
        }
        if (t->kind == IR_TERM_RET) {
            if (t->cond) {
                iv_copy(ret, ii_val(t->cond, NULL), false);
                IrInstr where; memset(&where, 0, sizeof where); where.line = t->line; where.col = t->col;
                where.unchecked = t->unchecked;                     // a `return` inside `unsafe`: it wraps
                ii_land(ret, t->cond->type, f->ret_type, &where);   // the result lands in the return type
            } else ret->k = IV_UNIT;
            break;
        }
        ii_fail(99, "PROOF FAILED", NULL, "control reached a point the compiler marked unreachable");
    }
    // The return value outlives this frame's temporaries: copy it into the caller's.
    IVal out; IAlloc *mine = ii_frame_allocs;
    ii_frame_allocs = saved_allocs;
    iv_copy(&out, ret, false); *ret = out;
    for (IObj *o = fr.objs; o; o = o->frame_next) {               // its storage is gone; the
        o->live = false; iv_free_owned(&o->root); o->root.n = 0;     // shell stays, so a dangling
    }                                                                // pointer is caught, not followed
    while (mine) { IAlloc *n = mine->next; free(mine->p); free(mine); mine = n; }
    ii_frame = caller; ii_cur_f = caller_f;
}

// Run `main`. Returns the process status the emitted C program should also have.
static IrFunc *ii_prepare(IrFunc *mod) __attribute__((noinline));
static IrFunc *ii_prepare(IrFunc *mod) {
    // The layout model reads which structs C sees (an extern's signature pins its order).
    ir_iface_n = 0;
    for (IrFunc *f = mod; f; f = f->next) {
        if (!f->is_extern) continue;
        ir_iface_mark(f->ret_type, 0);
        for (IrParam *p = f->params; p; p = p->next) if (p->value) ir_iface_mark(p->value->type, 0);
    }
    IrFunc *m = NULL;
    for (IrFunc *f = mod; f; f = f->next)
        if (!f->is_extern && f->name && f->name->length == 4 && memcmp(f->name->name, "main", 4) == 0) m = f;
    return m;
}
static int ir_interpret_module(IrFunc *mod, const char *file) {
    static IState s; memset(&s, 0, sizeof s);
    s.mod = mod; s.file = file; s.budget = 200000000LL;
    const char *b = getenv("LAIN_INTERP_STEPS"); if (b) s.budget = atoll(b);
    ist = &s;
    IrFunc *volatile m = ii_prepare(mod);
    if (!m) { fprintf(stderr, "lain --interpret: no `main`\n"); return 98; }
    if (setjmp(s.stop)) { fflush(stdout); ist = NULL; return s.status; }
    IVal ret; memset(&ret, 0, sizeof ret);
    ii_call(m, NULL, 0, &ret, NULL);
    fflush(stdout);
    int status = 0;
    if (ret.k == IV_INT) status = (int)(ret.i & 0xff);
    ist = NULL;
    return status;
}

// Run one function on given arguments: a compile-time THUNK (DECIDE-W step 2,
// src/frontends/lain/static_eval.h). On success *out holds the value, deep-copied into storage
// that outlives the run, and 0 is returned; otherwise the failure status (99 a proof failed,
// 98 not modelled, 97 the step budget, 96 undefined behaviour), after printing why. The budget is
// a STEP count, not time, so a constant that builds on one machine builds on every machine; *used
// receives the steps taken, so a caller can spread one budget over several calls.
static int ir_interpret_call(IrFunc *f, IrFunc *mod, const char *file, IVal *args, int nargs,
                             IVal *out, long long budget, long long *used) {
    static IState s; memset(&s, 0, sizeof s);
    s.mod = mod; s.file = file; s.budget = budget;
    s.who = "compile-time evaluation"; s.budget_is_callers = true;
    ist = &s;
    (void)ii_prepare(mod);
    // A failure leaves by longjmp from inside a call, past every ii_call epilogue, so the frame
    // globals still name a frame on a stack that is gone. The next call (compile-time evaluation
    // runs many) then attached its objects to it: stack-use-after-return under ASan (found by the
    // soundness harness's memory shapes). Reset them as a normal return would.
    if (setjmp(s.stop)) { ist = NULL; ii_frame = NULL; ii_cur_f = NULL; ii_frame_allocs = NULL; ii_open_seal = 0;
                          *used = s.steps; return s.status ? s.status : 98; }
    IVal ret; memset(&ret, 0, sizeof ret);
    ii_call(f, args, nargs, &ret, NULL);
    iv_copy(out, &ret, true);
    ist = NULL;
    *used = s.steps;
    return 0;
}

#endif // LAIN_IR_INTERP_H
