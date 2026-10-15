// src/tools/solve_vc.c — run the untrusted solver (src/solver/lia.h) on VC texts.
//
//   solve_vc FILE [PIVOT NODE LEAF]
//   solve_vc --smt FILE
//   solve_vc --max FILE [PIVOT NODE LEAF]
//   solve_vc --cone FILE [PIVOT NODE LEAF]
//
// Reads every VC in FILE (the text form of judge_interfaces.md §2, several in a row), solves each
// within the budget (default 100000 pivots, 4096 nodes, 4096 leaves) and prints the answer in
// the judge's own text: the certificate, the model, or `budget <id>` / `capacity <id>`, each
// followed by `work pivot:<n> node:<n> leaf:<n>`. The texts are the judge's (src/judge/text.h),
// so what a checker reads here is what the kernel will read.
//
// Every certificate and model is PARSED BACK by the judge's parser and must print back byte for
// byte: the kernel refuses a sum out of order or a non-canonical integer, and the solver
// normalises before it emits, so this is where that promise is checked (exit 4).
// `--smt` prints each VC and its SMT-LIB query instead, for an outside solver.
// `--max` maximizes each VC's goal SUM (its operator and constant are ignored) over its hypotheses
// (lia_maximize, PE.2): `max <id> <b>` then the certificate of `sum <= b` and the model attaining b;
// or `max-empty <id>` and the certificate refuting the hypotheses; or `max-none <id>` and a model;
// or `max-budget <id>` / `max-capacity <id>`.
// `--cone` answers each VC by lia_cone (a system closed under scaling, rank.h's sample LP), in the
// default form; a VC it does not accept is `capacity`.
// Exit 0, or 2 for a VC the parser refuses, 3 for memory, 4 for an answer that does not round-trip.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "judge/text.h"
#include "solver/lia.h"

static char region[1 << 24];
static char work[1 << 26];
static char back[1 << 22];

// The text `print` writes for `x`, NUL-terminated, in a malloc'd buffer.
static char *text_of(void (*print)(const void *, const JVc *, FILE *), const void *x, const JVc *v) {
    FILE *f = tmpfile(); if (!f) return NULL;
    print(x, v, f);
    long n = ftell(f); rewind(f);
    char *t = malloc((size_t)n + 1);
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) { free(t); t = NULL; }
    if (t) t[n] = 0;
    fclose(f);
    return t;
}
static void print_cert(const void *c, const JVc *v, FILE *o) { jcert_print((const JCert *)c, v, o); }
static void print_model(const void *m, const JVc *v, FILE *o) { jmodel_print((const JModel *)m, v, o); }

int main(int argc, char **argv) {
    bool smt = argc == 3 && strcmp(argv[1], "--smt") == 0;
    bool mx = (argc == 3 || argc == 6) && strcmp(argv[1], "--max") == 0;
    bool cone = (argc == 3 || argc == 6) && strcmp(argv[1], "--cone") == 0;
    if (argc != 2 && argc != 5 && !smt && !mx && !cone) {
        fprintf(stderr, "usage: solve_vc FILE [PIVOT NODE LEAF] | solve_vc --smt FILE | solve_vc --max|--cone FILE [PIVOT NODE LEAF]\n");
        return 2;
    }
    const char *path = smt || mx || cone ? argv[2] : argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    size_t cap = 1 << 16, n = 0; char *text = malloc(cap);
    for (size_t k; text && (k = fread(text + n, 1, cap - n - 1, f)) > 0; ) {
        n += k;
        if (n + 1 == cap) { cap *= 2; text = realloc(text, cap); }
    }
    fclose(f);
    if (!text) return 3;
    text[n] = 0;
    JWork lim = { 100000, 4096, 4096 };
    if (argc == 5) { lim.pivot = strtoull(argv[2], 0, 10); lim.node = strtoull(argv[3], 0, 10); lim.leaf = strtoull(argv[4], 0, 10); }
    if ((mx || cone) && argc == 6) { lim.pivot = strtoull(argv[3], 0, 10); lim.node = strtoull(argv[4], 0, 10); lim.leaf = strtoull(argv[5], 0, 10); }
    JMem m = jmem_over(region, sizeof region);
    JParse ps = jparse_over(&m, text);
    int rc = 0;
    while (*ps.p) {
        JVc v;
        if (!jvc_parse(&ps, &v)) { fprintf(stderr, "solve_vc: %s\n", ps.err); rc = 2; break; }
        if (smt) {
            jvc_print(&v, stdout); jvc_print_smtlib(&v, stdout);
            const char *next = ps.p; int line = ps.line;
            m = jmem_over(region, sizeof region);
            ps = jparse_over(&m, next); ps.line = line;
            continue;
        }
        if (mx) {
            LMax r = lia_maximize(&v, v.goal.t, v.goal.nt, lim, work, sizeof work);
            const char *tag = r.k == LIA_MAX_BOUND ? "max" : r.k == LIA_MAX_EMPTY ? "max-empty" : r.k == LIA_MAX_NONE ? "max-none"
                            : r.stop == J_ANS_BUDGET ? "max-budget" : "max-capacity";
            printf("%s ", tag); jid_print(&v.id, stdout);
            if (r.k == LIA_MAX_BOUND) { printf(" "); jint_print(&r.b, stdout); }
            printf("\n");
            if (r.k == LIA_MAX_BOUND || r.k == LIA_MAX_EMPTY) jcert_print(&r.cert, &r.vc, stdout);
            if (r.k == LIA_MAX_BOUND || r.k == LIA_MAX_NONE) jmodel_print(&r.model, &v, stdout);
            printf("work pivot:%llu node:%llu leaf:%llu\n", (unsigned long long)r.used.pivot,
                   (unsigned long long)r.used.node, (unsigned long long)r.used.leaf);
            const char *next = ps.p; int line = ps.line;
            m = jmem_over(region, sizeof region);
            ps = jparse_over(&m, next); ps.line = line;
            continue;
        }
        JAnswer a = cone ? lia_cone(&v, lim, work, sizeof work) : lia_solve(&v, lim, work, sizeof work);
        if (a.k == J_ANS_CERT || a.k == J_ANS_MODEL) {
            char *t = a.k == J_ANS_CERT ? text_of(print_cert, &a.cert, &v) : text_of(print_model, &a.model, &v);
            if (!t) { rc = 3; break; }
            fputs(t, stdout);
            JMem bm = jmem_over(back, sizeof back);
            JParse bp = jparse_over(&bm, t);
            JCert c2; JModel m2;
            bool ok = a.k == J_ANS_CERT ? jcert_parse(&bp, &v, &c2) : jmodel_parse(&bp, &v, &m2);
            char *t2 = ok && !*bp.p ? (a.k == J_ANS_CERT ? text_of(print_cert, &c2, &v) : text_of(print_model, &m2, &v)) : NULL;
            if (!t2 || strcmp(t, t2) != 0) {
                fprintf(stderr, "solve_vc: the answer does not round-trip through the judge's parser: %s\n", bp.err);
                rc = 4;
            }
            free(t); free(t2);
        } else { printf(a.k == J_ANS_BUDGET ? "budget " : "capacity "); jid_print(&v.id, stdout); printf("\n"); }
        printf("work pivot:%llu node:%llu leaf:%llu\n", (unsigned long long)a.used.pivot,
               (unsigned long long)a.used.node, (unsigned long long)a.used.leaf);
        // Each VC is parsed, answered and dropped: the next starts on an empty region, so a file
        // of thousands (a generator's dump of the corpus) needs no more than its largest VC.
        const char *next = ps.p; int line = ps.line;
        m = jmem_over(region, sizeof region);
        ps = jparse_over(&m, next); ps.line = line;
    }
    free(text);
    return rc;
}
