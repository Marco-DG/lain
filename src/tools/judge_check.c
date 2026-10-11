// src/tools/judge_check.c — the judge's verdict on the solver's answers: every certificate goes
// through the KERNEL (src/judge/kernel.h), so an obligation counts as implied only when the
// trusted kernel accepts the certificate the untrusted solver wrote.
//
//   judge_check VCS ANSWERS
//
// VCS is the text of several VCs in a row (judge_vc_driver.c's output, comment lines removed);
// ANSWERS is solve_vc's output for the same file: one answer per VC, in the same order, each a
// `cert` or `model` block (ending `end`) or a `budget`/`capacity` line, then a `work` line. One
// line per VC:
//   implied <id>                 the kernel accepted the certificate
//   REFUSED <id>: <reason>       the kernel refused it (a solver or kernel bug: never noise)
//   not-implied <id>             a model (the solver's; the kernel does not check models)
//   budget <id> / capacity <id>  the solver gave up
// and a last line `# judge_check vcs=<n> implied=<n> refused=<n> not-implied=<n> undecided=<n>`.
// Exit 0; 1 when any certificate is refused; 2 for input that does not parse or pair up.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "judge/format.h"
#include "judge/mem.h"
#include "judge/text.h"
#include "judge/kernel.h"

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}
// The next block starting with `vc ` (or the answer's first word) at or after *p: [*p, end).
static const char *next_line(const char *p) { const char *e = strchr(p, '\n'); return e ? e + 1 : p + strlen(p); }
static char region[1 << 26];

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s VCS ANSWERS\n", argv[0]); return 2; }
    char *vcs = slurp(argv[1]), *ans = slurp(argv[2]);
    if (!vcs || !ans) { fprintf(stderr, "judge_check: cannot read the inputs\n"); return 2; }
    const char *pv = vcs, *pa = ans;
    long n = 0, implied = 0, refused = 0, notimp = 0, undecided = 0;
    while (*pv) {
        // one VC: from `vc ` to its `end` line inclusive
        const char *vb = pv, *ve = pv;
        while (*ve && strncmp(ve, "end\n", 4) != 0) ve = next_line(ve);
        if (!*ve) { fprintf(stderr, "judge_check: a VC with no `end`\n"); return 2; }
        ve += 4;
        // its answer: up to and including the `work` line
        const char *ab = pa, *ae = pa;
        while (*ae && strncmp(ae, "work ", 5) != 0) ae = next_line(ae);
        if (!*ae) { fprintf(stderr, "judge_check: VC %ld has no answer\n", n + 1); return 2; }
        const char *aw = ae; ae = next_line(ae);
        pv = ve; pa = ae; n++;
        JMem m = jmem_over(region, sizeof region);
        char *vt = JMEM_NEW(&m, char, ve - vb + 1), *at = JMEM_NEW(&m, char, aw - ab + 1);
        if (!vt || !at) { fprintf(stderr, "judge_check: out of memory\n"); return 2; }
        memcpy(vt, vb, (size_t)(ve - vb)); vt[ve - vb] = 0;
        memcpy(at, ab, (size_t)(aw - ab)); at[aw - ab] = 0;
        JParse ps = jparse_over(&m, vt); JVc v;
        if (!jvc_parse(&ps, &v)) { fprintf(stderr, "judge_check: VC %ld: %s\n", n, ps.err); return 2; }
        char id[512]; snprintf(id, sizeof id, "%s %s %s %s %s", v.id.fn, v.id.irhash, v.id.point, v.id.kind, v.id.ordinal);
        if (!strncmp(at, "cert ", 5)) {
            JParse pc = jparse_over(&m, at); JCert c;
            if (!jcert_parse(&pc, &v, &c)) { printf("REFUSED %s: the certificate does not parse: %s\n", id, pc.err); refused++; continue; }
            if (strcmp(c.id.fn, v.id.fn) || strcmp(c.id.irhash, v.id.irhash) || strcmp(c.id.point, v.id.point) ||
                strcmp(c.id.kind, v.id.kind) || strcmp(c.id.ordinal, v.id.ordinal)) {
                printf("REFUSED %s: the certificate names another obligation\n", id); refused++; continue;
            }
            char why[256];
            if (jkernel_check(&m, &v, &c, why, sizeof why)) { printf("implied %s\n", id); implied++; }
            else { printf("REFUSED %s: %s\n", id, why); refused++; }
        } else if (!strncmp(at, "model ", 6)) { printf("not-implied %s\n", id); notimp++; }
        else if (!strncmp(at, "budget ", 7) || !strncmp(at, "capacity ", 9)) {
            printf("%s %s\n", !strncmp(at, "budget ", 7) ? "budget" : "capacity", id); undecided++;
        } else { fprintf(stderr, "judge_check: VC %ld: an answer of no known kind\n", n); return 2; }
    }
    if (*pa) { fprintf(stderr, "judge_check: more answers than VCs\n"); return 2; }
    printf("# judge_check vcs=%ld implied=%ld refused=%ld not-implied=%ld undecided=%ld\n", n, implied, refused, notimp, undecided);
    return refused ? 1 : 0;
}
