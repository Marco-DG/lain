// src/tools/judge_vc_driver.c — phase 1a of the proof engine's judge: the VCs the generator
// (src/judge/gen.h) states for every obligation of a Lain file, beside VRA's verdict on the same
// instruction. Nothing here decides anything: the judge has no kernel yet and no solver runs.
//
//   gcc -std=c99 -O2 -o /tmp/jvc src/tools/judge_vc_driver.c -I src
//   /tmp/jvc FILE.ln            each obligation: a header line, then its VC in the text form
//   /tmp/jvc FILE.ln --smt      the same, as SMT-LIB (QF_LIA), for an offline solver
//   /tmp/jvc FILE.ln --stats    one TSV line per obligation: function, point, kind, ordinal,
//                               VRA's verdict, outside or stated, #vars, #hyps
//   /tmp/jvc FILE.ln --check-hyps
//                               run the program on the interpreter and, each time an obligation's
//                               instruction is reached, evaluate every hypothesis of its VC at the
//                               values the program has there. The generator is TRUSTED: a false
//                               hypothesis is a generator bug (or the interpreter's), never noise.
//                               One line per false hypothesis, then a summary line, on STDERR:
//                               the program's own output is on stdout and need not end a line.
//
// VRA's verdict is read from its checks on the same instruction: `proved` when VRA discharged the
// matching half, `refused` when it did not, `none` when VRA states no obligation there.
// A driver, not the judge: it may include src/analysis/ (the include rules bind src/judge/).
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/file.h"
#include "utils/common/system.h"
#include "utils/panic.h"
#include <unistd.h>
#include "frontends/lain/lexer.h"
#include "frontends/lain/parser.h"
#include "frontends/lain/ast.h"
#include "frontends/lain/module.h"
#include "target.h"
#include "frontends/lain/sema.h"
#include "frontends/lain/lower.h"
#include "analysis/vra.h"
#include "ir/emit_c.h"     // interp.h reads ir_iface_mark from it, as main.c includes them
#include "ir/interp.h"
#include "judge/gen.h"
#include "judge/text.h"

// The module name as the compiler makes it (main.c, filepath_to_modname): one dotted segment per
// path component, each an identifier. The program itself is read from the path as given
// (module_root_file). The plainer name vra_driver.c uses made a program that names its own
// module's declarations fail with E105.
static char *drv_modname(Arena *a, const char *path) {
    const char *p = path;
    while ((p[0]=='.' && (p[1]=='/'||p[1]=='\\')) || p[0]=='/' || p[0]=='\\')
        p += (p[0]=='/'||p[0]=='\\') ? 1 : 2;
    size_t n = strlen(p), end = (n>3 && strcmp(p+n-3,".ln")==0) ? n-3 : n;
    char *t = arena_push_many(a, char, 2*end+1);
    size_t o = 0; bool seg_start = true;
    for (size_t i = 0; i < end; i++) {
        char c = p[i];
        if (c == '/' || c == '\\') { t[o++] = '.'; seg_start = true; continue; }
        bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (seg_start && c >= '0' && c <= '9') t[o++] = '_';
        t[o++] = ident ? c : '_';
        seg_start = false;
    }
    t[o] = '\0';
    return t;
}

// VRA's verdict on the half of an obligation the judge states. VRA keeps one check per
// instruction and kind; its lo_ok/hi_ok split the bounds check, `shift` and `refine` qualify an
// overflow check. A judge kind with no VRA counterpart at this instruction is `none`.
static const char *vra_verdict(Vra *V, JObl *o) {
    const char *k = o->vc.id.kind ? o->vc.id.kind : "";
    for (int i = 0; i < V->nchecks; i++) {
        VraCheck *c = &V->checks[i];
        if (c->at != o->at) continue;
        if (!strcmp(k, "bounds.lo") && c->kind == VRA_BOUNDS) return c->lo_ok || c->ok ? "proved" : "refused";
        if (!strcmp(k, "bounds.hi") && c->kind == VRA_BOUNDS) return c->hi_ok || c->ok ? "proved" : "refused";
        if ((!strcmp(k, "divzero") || !strcmp(k, "divovf")) && c->kind == VRA_DIVZERO) return c->ok ? "proved" : "refused";
        if ((!strncmp(k, "narrow", 6) || !strcmp(k, "shift")) && c->kind == VRA_OVERFLOW) return c->ok ? "proved" : "refused";
    }
    return "none";
}

// ── --check-hyps ───────────────────────────────────────────────────────────────────────────
typedef struct { IrInstr *at; JObl *o; } HK;
static HK *hk; static int64_t nhk;
static long long reached, checked, falsec, unknown, premises;
static int hk_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const HK *)a)->at, y = (uintptr_t)((const HK *)b)->at;
    return x < y ? -1 : x > y;
}
// A variable's value at the instruction, or false when it cannot be read (not an integer yet).
static bool hk_var(JObl *o, int32_t x, IVal *v, int nv, __int128 *out) {
    int32_t k = o->vkind[x], key = o->vkey[x];
    if (key < 0 || key >= nv) return false;
    // the obligation's own result does not exist yet: the hook runs before the instruction, and
    // the VC's variable for it is the exact value the instruction is about to settle
    if (k == 0 && o->at->result && key == o->at->result->id) return false;
    IVal *c = &v[key];
    if (k == 1) {
        // the version current at the point is the cell's content; an older one is the value its
        // store wrote, which the frame still holds (the store and that value dominate the point)
        if (o->vcur[x]) {
            if (c->k != IV_PTR || !c->p.o || !c->p.o->live) return false;
            c = ip_cell(&c->p, NULL);
        } else {
            if (o->vaux[x] < 0 || o->vaux[x] >= nv) return false;
            c = &v[o->vaux[x]];
        }
    }
    if (k == 2) { if (c->k != IV_SLICE) return false; *out = c->len; return true; }
    if (k == 3) {                                      // a fresh variable: its recipe
        const JRecipe *r = o->vrec ? o->vrec[x] : NULL;
        if (!r || r->den == 0) return false;
        __int128 s = 0;
        for (int i = 0; i < r->n; i++) {
            __int128 y, p;
            if (r->var[i] == x || !hk_var(o, r->var[i], v, nv, &y)) return false;
            if (__builtin_mul_overflow(r->coef[i], y, &p) || __builtin_add_overflow(s, p, &s)) return false;
        }
        if (s % r->den) return false;                  // not exact: the recipe does not apply
        *out = s / r->den; return true;
    }
    if (c->k != IV_INT) return false;
    const IrType *t = o->vtype[x];
    bool uns = t && ((t->kind == IRT_INT && !t->is_signed) || t->kind == IRT_BOOL);
    *out = uns ? (__int128)c->i : (__int128)(int64_t)c->i;
    return true;
}
static __int128 hk_jint(const JInt *x) {             // the generator's constants are below 2^66
    unsigned __int128 m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    return x->neg ? -(__int128)m : (__int128)m;
}
// 1 true, 0 false, -1 not evaluable
static int hk_cons(JObl *o, const JCons *c, IVal *v, int nv) {
    __int128 s = 0;
    for (int i = 0; i < c->nt; i++) {
        __int128 x, p;
        if (!hk_var(o, c->t[i].var, v, nv, &x)) return -1;
        if (__builtin_mul_overflow(hk_jint(&c->t[i].coef), x, &p) || __builtin_add_overflow(s, p, &s)) return -1;
    }
    __int128 r = hk_jint(&c->rhs);
    return c->op == J_LE ? s <= r : c->op == J_EQ ? s == r : s != r;
}
static void hk_on_instr(IrFunc *f, IrBlock *b, IrInstr *i, IVal *v, int nv) {
    (void)f; (void)b;
    if (!i || !nhk) return;
    HK key = { i, NULL };
    HK *h = bsearch(&key, hk, (size_t)nhk, sizeof *hk, hk_cmp);
    if (!h) return;
    while (h > hk && h[-1].at == i) h--;
    for (; h < hk + nhk && h->at == i; h++) {
        JObl *o = h->o; reached++;
        for (int32_t k = 0; k < o->vc.nhyps; k++) {
            const JHyp *y = &o->vc.hyps[k];
            int r;
            // a premise is the antecedent of an implication VC (divovf: x = i64.min ⊨ y != -1),
            // not a fact about the run
            if (!strncmp(y->origin, "premise ", 8)) { premises++; continue; }
            if (!y->disj) r = hk_cons(o, &y->cons, v, nv);
            else {
                r = 0;
                for (int d = 0; d < y->ndis && r != 1; d++) {
                    int all = 1;
                    for (int c = 0; c < y->dis[d].nc && all == 1; c++) { int t = hk_cons(o, &y->dis[d].c[c], v, nv); all = t == 1 ? 1 : t; }
                    if (all == 1) r = 1; else if (all < 0) r = -1;
                }
            }
            if (r < 0) { unknown++; continue; }
            checked++;
            if (r == 1) continue;
            falsec++;
            fprintf(stderr, "FALSE %s %s %s %s h%d (%s):", o->vc.id.fn, o->vc.id.point, o->vc.id.kind, o->vc.id.ordinal, k + 1, y->origin);
            for (int32_t x = 0; x < o->vc.nvars; x++) {
                __int128 val;
                if (hk_var(o, x, v, nv, &val)) fprintf(stderr, " %s=%lld", o->vc.vars[x], (long long)val);
            }
            fprintf(stderr, "\n");
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.ln> [--smt | --stats]\n", argv[0]); return 2; }
    bool smt = false, stats = false, check = false, why = false;
    for (int k = 2; k < argc; k++) {
        if (!strcmp(argv[k], "--smt")) smt = true;
        if (!strcmp(argv[k], "--stats")) stats = true;
        if (!strcmp(argv[k], "--check-hyps")) check = true;
        if (!strcmp(argv[k], "--why")) why = true;
    }
    Arena aa = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena sa = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ia = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    target_init_for(NULL);
    const char *path = argv[1];
    char *mod = drv_modname(&aa, path);
    module_root_file = path;
    DeclList *prog = load_module(&aa, mod);
    if (!prog) { fprintf(stderr, "load failed\n"); return 1; }
    sema_resolve_module(prog, mod, &sa);
    IrFunc *irmod = ir_lower_module(prog, &ia);
    vra_mod = irmod;
    size_t cap = (size_t)1 << 28;
    char *buf = malloc(cap);
    if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }
    int rc = 0;
    JMem m = jmem_over(buf, cap);                      // one region: --check-hyps keeps every VC
    for (IrFunc *f = irmod; f; f = f->next) {
        if (f->is_extern) continue;
        if (check) {
            JObl *o; int32_t n; char err[200];
            if (!jgen_func(&m, f, &o, &n, err, sizeof err)) {
                fprintf(stderr, "ERROR %.*s: %s\n", (int)f->name->length, f->name->name, err); rc = 1; continue;
            }
            HK *nh = realloc(hk, sizeof *hk * (size_t)(nhk + n + 1));
            if (!nh) { fprintf(stderr, "out of memory\n"); return 1; }
            hk = nh;
            for (int32_t i = 0; i < n; i++) if (!o[i].outside) { hk[nhk].at = o[i].at; hk[nhk].o = &o[i]; nhk++; }
            continue;
        }
        Vra *V = vra_analyze(f);
        m = jmem_over(buf, cap);
        JObl *o; int32_t n; char err[200];
        if (!jgen_func(&m, f, &o, &n, err, sizeof err)) {
            printf("%s %.*s: %s\n", stats ? "ERROR\t" : "# generator refused", (int)f->name->length, f->name->name, err);
            rc = 1; continue;
        }
        for (int32_t i = 0; i < n; i++) {
            const char *vv = o[i].outside ? "-" : vra_verdict(V, &o[i]);
            if (why) {
                // what the goal's variables are: the step that would have to state more about them
                if (o[i].outside) continue;
                printf("%s %s %s %s %s\t", o[i].vc.id.fn, o[i].vc.id.irhash, o[i].vc.id.point, o[i].vc.id.kind, o[i].vc.id.ordinal);
                for (int t = 0; t < o[i].vc.goal.nt; t++) {
                    int32_t x = o[i].vc.goal.t[t].var;
                    const char *what = "?";
                    if (o[i].vkind[x] == 2) what = "len";
                    else if (o[i].vkind[x] == 1) {
                        const char *nmx = o[i].vc.vars[x];
                        what = strstr(nmx, "@head") ? "version.head" : strstr(nmx, "@join") ? "version.join"
                             : strstr(nmx, "@entry") ? "version.entry" : "version.store";
                    } else {
                        IrInstr *d = NULL;
                        for (IrBlock *bb = f->blocks; bb && !d; bb = bb->next)
                            for (IrInstr *ii = bb->instrs; ii; ii = ii->next)
                                if (ii->result && ii->result->id == o[i].vkey[x]) { d = ii; break; }
                        static const char *opn[] = { [IR_CONST]="const", [IR_CAST]="cast", [IR_ADD]="add", [IR_SUB]="sub", [IR_MUL]="mul",
                            [IR_SDIV]="div", [IR_UDIV]="div", [IR_SREM]="rem", [IR_UREM]="rem", [IR_NEG]="neg", [IR_AND]="and", [IR_OR]="or",
                            [IR_XOR]="xor", [IR_SHL]="shl", [IR_LSHR]="lshr", [IR_ASHR]="ashr", [IR_BNOT]="bnot", [IR_CTZ]="ctz", [IR_CLZ]="clz",
                            [IR_POPCOUNT]="popcount", [IR_SIZEOF]="sizeof", [IR_ALIGNOF]="alignof", [IR_ICMP]="icmp", [IR_LOAD]="load",
                            [IR_SLICE_LEN]="slice_len", [IR_CALL]="call", [IR_SUM_TAG]="sum_tag", [IR_SUM_PAYLOAD]="sum_payload",
                            [IR_VEC_MOVEMASK]="movemask", [IR_SEQ_EQ]="seq_eq" };
                        if (!d) what = "param";
                        else if ((size_t)d->op < sizeof opn / sizeof *opn && opn[d->op]) what = opn[d->op];
                        else what = "other-op";
                        if (d && d->op == IR_LOAD) {
                            IrInstr *ad = NULL; IrValue *a0 = d->operands[0];
                            for (IrBlock *bb = f->blocks; bb && !ad; bb = bb->next)
                                for (IrInstr *ii = bb->instrs; ii; ii = ii->next)
                                    if (ii->result == a0) { ad = ii; break; }
                            what = !ad ? "load.param-ptr" : ad->op == IR_ALLOCA ? "load.escaped-local" : ad->op == IR_FIELD_PTR ? "load.field"
                                 : ad->op == IR_ELEM_PTR ? "load.element" : "load.other";
                        }
                    }
                    printf("%s%s", t ? "," : "", what);
                }
                printf("\n");
                continue;
            }
            if (stats) {
                if (o[i].outside) printf("%.*s\tbb%d.%d\toutside\t-\t%s\t%s\n", (int)f->name->length, f->name->name,
                                         o[i].blk->id, o[i].idx, vra_verdict(V, &o[i]), o[i].outside);
                else printf("%s\t%s\t%s\t%s\t%s\tstated\t%d\t%d\n", o[i].vc.id.fn, o[i].vc.id.point, o[i].vc.id.kind,
                            o[i].vc.id.ordinal, vv, o[i].vc.nvars, o[i].vc.nhyps);
                continue;
            }
            if (o[i].outside) {
                printf("# %.*s bb%d.%d outside: %s\n", (int)f->name->length, f->name->name, o[i].blk->id, o[i].idx, o[i].outside);
                continue;
            }
            printf("# vra=%s\n", vv);
            if (smt) jvc_print_smtlib(&o[i].vc, stdout); else jvc_print(&o[i].vc, stdout);
        }
        vra_free(V);
    }
    if (check) {
        qsort(hk, (size_t)nhk, sizeof *hk, hk_cmp);
        ii_on_instr = hk_on_instr;
        int st = ir_interpret_module(irmod, path);
        fflush(stdout);
        fprintf(stderr, "SUMMARY obligations=%lld reached=%lld hyps-checked=%lld false=%lld not-evaluable=%lld premises=%lld status=%d\n",
               (long long)nhk, reached, checked, falsec, unknown, premises, st);
        if (falsec) rc = 3;
    }
    free(buf);
    return rc;
}
