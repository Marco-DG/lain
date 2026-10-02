#ifndef LAIN_ANALYSIS_CERTIFICATE_H
#define LAIN_ANALYSIS_CERTIFICATE_H
// certificate.h — a proof certificate: what the range analysis FOUND, written so that a checker
// can verify it in one pass without searching (local/internal/design/certificates.md, C.2).
//
// The analysis splits into a search (fixpoints, widening, loop discovery, measure search) and a
// check. A certificate is the search's output, per function:
//   - the constant table, the values held outside the octagon (`V->cknown`);
//   - one CLOSED octagon state per loop header, its finite entries only;
//   - the termination measures: per loop, per self-recursion, per mutual 2-cycle;
//   - the side facts a proof consulted that no block state holds (M5c): element ranges per array
//     cell, the function's return range, call-site return ranges with the callee's certificate
//     under the call's constant arguments NESTED, and the accumulator bounds (B1).
// This file is the format: the structures, the printer and the parser. It knows nothing of the
// analysis, so the checker (C.3) can read a certificate without linking the search. vra.h fills
// the structures (vra_cert_build).
//
// The text is line-oriented and meant to be read by a person:
//
//   certificate count
//     const %3 = 0
//     header bb1
//       %2:i in [0, +inf]
//       %2:i - %0:n <= 0
//     measure loop bb1 rises %2:i %0:n
//     ret in [0, 100]
//   end
//
// A value is `%id`, with its source name after a colon where lowering recorded one; the name is
// for the reader, and the parser keeps it only so that a re-print is identical. A binary
// constraint is one of `a - b <= c`, `a + b <= c`, `-a - b <= c`: exactly the octagon's entries,
// each coherent pair once.
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CERT_NAME 64
typedef struct { int id; char name[CERT_NAME]; } CertRef;
typedef struct { bool has; int64_t v; } CertBound;                  // has = false: infinite
typedef struct { CertRef v; CertBound lo, hi; } CertIv;
typedef enum { CERT_DIFF, CERT_SUM, CERT_NSUM } CertBinKind;      // a - b, a + b, -a - b
typedef struct { CertBinKind k; CertRef a, b; int64_t c; } CertBin;
typedef struct {
    int block; bool bottom;
    CertIv *iv; int niv, civ;
    CertBin *bin; int nbin, cbin;
} CertHeader;
typedef struct { CertRef v; int64_t c; } CertConst;
typedef enum { CERT_M_LOOP, CERT_M_REC, CERT_M_MUTUAL } CertMKind;
typedef struct {
    CertMKind k;
    int block;                   // LOOP: the header
    char rule[16];               // rises falls pair none | param param_diff none | mutual
    bool has_a, has_b; CertRef a, b;
    char *other;                 // MUTUAL: the other function of the 2-cycle
    int kf, kg;                  // MUTUAL: the parameter positions in this function and the other
    char strict;                 // MUTUAL: the edge that strictly falls, 'f' (this one's call) or 'g'
} CertMeasure;
typedef struct { CertRef v; CertBound lo, hi; } CertElem;
typedef struct {
    CertRef v; int block;        // the accumulator value, and the loop header it accumulates in
    int64_t trips, dlo, dhi, s0lo, s0hi;
} CertAccum;
struct CertFunc;
typedef struct {
    CertRef result; char *callee;
    int nbind; int bind_k[16]; int64_t bind_lo[16], bind_hi[16];
    CertBound lo, hi;
    struct CertFunc *sub;        // the callee's certificate under these arguments (NULL: none)
} CertCall;
typedef struct CertFunc {
    char *name;                  // a function's name has no length limit (a generic instance's is long)
    CertConst *consts; int nconst, cconst;
    CertHeader *hdr; int nhdr, chdr;
    CertMeasure *meas; int nmeas, cmeas;
    CertElem *elem; int nelem, celem;
    bool has_ret; CertBound ret_lo, ret_hi;
    CertAccum *acc; int nacc, cacc;
    CertCall *call; int ncall, ccall;
    struct CertFunc *next;       // the next certificate of a file
} CertFunc;

// ── building ─────────────────────────────────────────────────────────────────────────────────
#define CERT_PUSH(arr, n, cap) \
    ((n) >= (cap) ? ((cap) = (cap) ? 2*(cap) : 8, (arr) = realloc((arr), (size_t)(cap)*sizeof *(arr))) : (arr), \
     memset(&(arr)[(n)], 0, sizeof *(arr)), &(arr)[(n)++])
static char *cert_strdup(const char *s, int len) {
    if (!s) { s = ""; len = 0; }
    if (len < 0) len = (int)strlen(s);
    char *d = malloc((size_t)len + 1); memcpy(d, s, (size_t)len); d[len] = 0;
    return d;
}
static CertFunc *cert_new(const char *name, int len) {
    CertFunc *c = calloc(1, sizeof *c);
    c->name = cert_strdup(name, len);
    return c;
}
// A source name goes into the text only when the parser can read it back as one token.
static void cert_set_name(CertRef *r, const char *s, int len) {
    r->name[0] = 0;
    if (!s || len <= 0 || len >= CERT_NAME) return;
    for (int i = 0; i < len; i++) {
        char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '.')) return;
    }
    memcpy(r->name, s, (size_t)len); r->name[len] = 0;
}
static void cert_free(CertFunc *c) {
    while (c) {
        CertFunc *nx = c->next;
        for (int i = 0; i < c->nhdr; i++) { free(c->hdr[i].iv); free(c->hdr[i].bin); }
        for (int i = 0; i < c->ncall; i++) { cert_free(c->call[i].sub); free(c->call[i].callee); }
        for (int i = 0; i < c->nmeas; i++) free(c->meas[i].other);
        free(c->name);
        free(c->consts); free(c->hdr); free(c->meas); free(c->elem); free(c->acc); free(c->call);
        free(c); c = nx;
    }
}

// ── printing ─────────────────────────────────────────────────────────────────────────────────
static void cert_ref(const CertRef *r, FILE *o) {
    if (r->name[0]) fprintf(o, "%%%d:%s", r->id, r->name); else fprintf(o, "%%%d", r->id);
}
static void cert_bound(CertBound b, bool upper, FILE *o) {
    if (b.has) fprintf(o, "%lld", (long long)b.v); else fputs(upper ? "+inf" : "-inf", o);
}
static void cert_range(CertBound lo, CertBound hi, FILE *o) {
    fputc('[', o); cert_bound(lo, false, o); fputs(", ", o); cert_bound(hi, true, o); fputc(']', o);
}
static void cert_print_at(const CertFunc *c, FILE *o, int ind) {
    fprintf(o, "%*scertificate %s\n", ind, "", c->name);
    int in = ind + 2;
    for (int i = 0; i < c->nconst; i++) {
        fprintf(o, "%*sconst ", in, ""); cert_ref(&c->consts[i].v, o);
        fprintf(o, " = %lld\n", (long long)c->consts[i].c);
    }
    for (int h = 0; h < c->nhdr; h++) {
        const CertHeader *H = &c->hdr[h];
        fprintf(o, "%*sheader bb%d%s\n", in, "", H->block, H->bottom ? " bottom" : "");
        for (int i = 0; i < H->niv; i++) {
            fprintf(o, "%*s", in + 2, ""); cert_ref(&H->iv[i].v, o); fputs(" in ", o);
            cert_range(H->iv[i].lo, H->iv[i].hi, o); fputc('\n', o);
        }
        for (int i = 0; i < H->nbin; i++) {
            const CertBin *B = &H->bin[i];
            fprintf(o, "%*s", in + 2, "");
            if (B->k == CERT_NSUM) fputc('-', o);
            cert_ref(&B->a, o);
            fputs(B->k == CERT_SUM ? " + " : " - ", o);
            cert_ref(&B->b, o);
            fprintf(o, " <= %lld\n", (long long)B->c);
        }
    }
    for (int i = 0; i < c->nmeas; i++) {
        const CertMeasure *M = &c->meas[i];
        fprintf(o, "%*smeasure ", in, "");
        if (M->k == CERT_M_LOOP) fprintf(o, "loop bb%d %s", M->block, M->rule);
        else if (M->k == CERT_M_REC) fprintf(o, "recursion %s", M->rule);
        else fprintf(o, "mutual with %s params %d %d strict %c", M->other, M->kf, M->kg, M->strict);
        if (M->k != CERT_M_MUTUAL && M->has_a) { fputc(' ', o); cert_ref(&M->a, o); }
        if (M->k != CERT_M_MUTUAL && M->has_b) { fputc(' ', o); cert_ref(&M->b, o); }
        fputc('\n', o);
    }
    for (int i = 0; i < c->nelem; i++) {
        fprintf(o, "%*selem ", in, ""); cert_ref(&c->elem[i].v, o); fputs(" in ", o);
        cert_range(c->elem[i].lo, c->elem[i].hi, o); fputc('\n', o);
    }
    if (c->has_ret) { fprintf(o, "%*sret in ", in, ""); cert_range(c->ret_lo, c->ret_hi, o); fputc('\n', o); }
    for (int i = 0; i < c->nacc; i++) {
        const CertAccum *A = &c->acc[i];
        fprintf(o, "%*saccum ", in, ""); cert_ref(&A->v, o);
        fprintf(o, " header bb%d trips %lld delta [%lld, %lld] init [%lld, %lld]\n", A->block,
                (long long)A->trips, (long long)A->dlo, (long long)A->dhi, (long long)A->s0lo,
                (long long)A->s0hi);
    }
    for (int i = 0; i < c->ncall; i++) {
        const CertCall *K = &c->call[i];
        fprintf(o, "%*scallsite ", in, ""); cert_ref(&K->result, o);
        fprintf(o, " callee %s bind", K->callee);
        for (int j = 0; j < K->nbind; j++)
            fprintf(o, " %d=[%lld, %lld]", K->bind_k[j], (long long)K->bind_lo[j], (long long)K->bind_hi[j]);
        fputs(" ret in ", o); cert_range(K->lo, K->hi, o); fputc('\n', o);
        if (K->sub) cert_print_at(K->sub, o, in + 2);
    }
    fprintf(o, "%*send\n", ind, "");
}
static void cert_print(const CertFunc *c, FILE *o) { for (; c; c = c->next) cert_print_at(c, o, 0); }

// ── parsing ──────────────────────────────────────────────────────────────────────────────────
// One line at a time, into the same structures the analysis fills, so printing what was parsed
// reproduces the file (the round trip is the format's test). A malformed line is an error with
// its line number: a checker must never guess.
#define CERT_LINE 65536
#define CERT_TOKS 256
typedef struct { char *t[CERT_TOKS]; int n; bool overflow; char buf[2*CERT_LINE]; } CertLine;
static void cert_tokenize(const char *s, CertLine *L) {
    L->n = 0; L->overflow = false;
    char *o = L->buf;
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') { s++; continue; }
        if (L->n >= CERT_TOKS) { L->overflow = true; return; }
        L->t[L->n++] = o;
        if (*s == '[' || *s == ']' || *s == ',' || *s == '=') *o++ = *s++;
        else if (*s == '<' && s[1] == '=') { *o++ = '<'; *o++ = '='; s += 2; }
        else
            while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '[' &&
                   *s != ']' && *s != ',' && *s != '=' && !(*s == '<' && s[1] == '=')) *o++ = *s++;
        *o++ = 0;
    }
}
static bool cert_int(const char *t, int64_t *v) {
    char *e; long long x = strtoll(t, &e, 10);
    if (!*t || *e) return false;
    *v = x; return true;
}
static bool cert_parse_ref(const char *t, CertRef *r) {
    if (t[0] != '%') return false;
    char *e; long x = strtol(t + 1, &e, 10);
    if (e == t + 1) return false;
    r->id = (int)x; r->name[0] = 0;
    if (*e == ':') { int n = (int)strlen(e + 1); if (n <= 0 || n >= CERT_NAME) return false; memcpy(r->name, e + 1, (size_t)n + 1); }
    else if (*e) return false;
    return true;
}
static bool cert_parse_bound(const char *t, CertBound *b) {
    if (!strcmp(t, "-inf") || !strcmp(t, "+inf")) { b->has = false; b->v = 0; return true; }
    b->has = true; return cert_int(t, &b->v);
}
// `[ lo , hi ]` starting at token i; returns the index after it, or -1
static int cert_parse_range(CertLine *L, int i, CertBound *lo, CertBound *hi) {
    if (i < 0 || i + 4 >= L->n) return -1;
    if (strcmp(L->t[i], "[") || strcmp(L->t[i+2], ",") || strcmp(L->t[i+4], "]")) return -1;
    if (!cert_parse_bound(L->t[i+1], lo) || !cert_parse_bound(L->t[i+3], hi)) return -1;
    return i + 5;
}
static int cert_parse_irange(CertLine *L, int i, int64_t *lo, int64_t *hi) {
    CertBound a, b; int j = cert_parse_range(L, i, &a, &b);
    if (j < 0 || !a.has || !b.has) return -1;
    *lo = a.v; *hi = b.v; return j;
}
static bool cert_parse_block(const char *t, int *b) {
    int64_t v; if (strncmp(t, "bb", 2) || !cert_int(t + 2, &v)) return false;
    *b = (int)v; return true;
}
static bool cert_copy_name(char *dst, const char *t, size_t cap) {
    size_t n = strlen(t); if (n == 0 || n >= cap) return false;
    memcpy(dst, t, n + 1); return true;
}
// Returns the certificates of the file in order, or NULL with *err set ("line N: ...").
static CertFunc *cert_parse(FILE *in, char *err, size_t errn) {
    CertFunc *head = NULL, **tail = &head;
    CertFunc *stack[8]; int sp = 0;            // the certificate being filled, and its parents
    CertCall *pending = NULL;                  // a callsite whose nested certificate comes next
    CertHeader *hdr = NULL;                    // the header that constraint lines belong to
    static char buf[CERT_LINE]; int lineno = 0; static CertLine L;
    // every certificate made so far is reachable from head (a nested one hangs off its callsite)
#define CERT_BAD(msg) do { snprintf(err, errn, "line %d: %s", lineno, msg); cert_free(head); return NULL; } while (0)
    while (fgets(buf, sizeof buf, in)) {
        lineno++;
        if (strlen(buf) == sizeof buf - 1 && buf[sizeof buf - 2] != '\n') CERT_BAD("a line too long");
        cert_tokenize(buf, &L);
        if (L.overflow) CERT_BAD("too many tokens on a line");
        if (L.n == 0) continue;
        const char *k0 = L.t[0];
        if (!strcmp(k0, "certificate")) {
            if (L.n != 2) CERT_BAD("certificate takes one name");
            if (sp >= 8) CERT_BAD("certificates nested too deep");
            CertFunc *c = cert_new(L.t[1], -1);
            if (sp == 0) { *tail = c; tail = &c->next; }
            else {
                if (!pending) { cert_free(c); CERT_BAD("a nested certificate must follow a callsite"); }
                pending->sub = c; pending = NULL;
            }
            stack[sp++] = c; hdr = NULL; continue;
        }
        if (sp == 0) CERT_BAD("a line outside any certificate");
        CertFunc *c = stack[sp-1];
        if (pending) CERT_BAD("a callsite must be followed by the callee's certificate");
        if (!strcmp(k0, "end")) {
            if (L.n != 1) CERT_BAD("end takes nothing");
            sp--; hdr = NULL; continue;
        }
        if (k0[0] == '%' || (k0[0] == '-' && k0[1] == '%')) {        // a constraint of the last header
            if (!hdr) CERT_BAD("a constraint outside a header");
            if (L.n == 7 && !strcmp(L.t[1], "in")) {
                CertIv *v = CERT_PUSH(hdr->iv, hdr->niv, hdr->civ);
                if (!cert_parse_ref(k0, &v->v) || cert_parse_range(&L, 2, &v->lo, &v->hi) != 7) CERT_BAD("bad interval");
                continue;
            }
            if (L.n != 5 || strcmp(L.t[3], "<=")) CERT_BAD("bad constraint");
            CertBin *b = CERT_PUSH(hdr->bin, hdr->nbin, hdr->cbin);
            bool neg = (k0[0] == '-');
            if (!cert_parse_ref(k0 + (neg ? 1 : 0), &b->a) || !cert_parse_ref(L.t[2], &b->b) ||
                !cert_int(L.t[4], &b->c)) CERT_BAD("bad constraint");
            if (!strcmp(L.t[1], "-")) b->k = neg ? CERT_NSUM : CERT_DIFF;
            else if (!strcmp(L.t[1], "+") && !neg) b->k = CERT_SUM;
            else CERT_BAD("bad constraint operator");
            continue;
        }
        hdr = NULL;
        if (!strcmp(k0, "const")) {
            CertConst *k = CERT_PUSH(c->consts, c->nconst, c->cconst);
            if (L.n != 4 || !cert_parse_ref(L.t[1], &k->v) || strcmp(L.t[2], "=") || !cert_int(L.t[3], &k->c))
                CERT_BAD("bad const");
        } else if (!strcmp(k0, "header")) {
            CertHeader *H = CERT_PUSH(c->hdr, c->nhdr, c->chdr);
            if ((L.n != 2 && L.n != 3) || !cert_parse_block(L.t[1], &H->block)) CERT_BAD("bad header");
            if (L.n == 3) { if (strcmp(L.t[2], "bottom")) CERT_BAD("bad header"); H->bottom = true; }
            hdr = H;
        } else if (!strcmp(k0, "measure")) {
            CertMeasure *M = CERT_PUSH(c->meas, c->nmeas, c->cmeas);
            int i;
            if (L.n >= 3 && !strcmp(L.t[1], "loop")) {
                M->k = CERT_M_LOOP;
                if (L.n < 4 || !cert_parse_block(L.t[2], &M->block) || !cert_copy_name(M->rule, L.t[3], sizeof M->rule)) CERT_BAD("bad loop measure");
                i = 4;
            } else if (L.n >= 3 && !strcmp(L.t[1], "recursion")) {
                M->k = CERT_M_REC; if (!cert_copy_name(M->rule, L.t[2], sizeof M->rule)) CERT_BAD("bad recursion measure");
                i = 3;
            } else if (L.n == 9 && !strcmp(L.t[1], "mutual") && !strcmp(L.t[2], "with") &&
                       !strcmp(L.t[4], "params") && !strcmp(L.t[7], "strict")) {
                int64_t a, b;
                M->k = CERT_M_MUTUAL; strcpy(M->rule, "mutual");
                M->other = cert_strdup(L.t[3], -1);
                if (!cert_int(L.t[5], &a) || !cert_int(L.t[6], &b) ||
                    strlen(L.t[8]) != 1 || (L.t[8][0] != 'f' && L.t[8][0] != 'g')) CERT_BAD("bad mutual measure");
                M->kf = (int)a; M->kg = (int)b; M->strict = L.t[8][0];
                continue;
            } else CERT_BAD("bad measure");
            if (i < L.n) { if (!cert_parse_ref(L.t[i], &M->a)) CERT_BAD("bad measure value"); M->has_a = true; i++; }
            if (i < L.n) { if (!cert_parse_ref(L.t[i], &M->b)) CERT_BAD("bad measure value"); M->has_b = true; i++; }
            if (i != L.n) CERT_BAD("bad measure");
        } else if (!strcmp(k0, "elem")) {
            CertElem *E = CERT_PUSH(c->elem, c->nelem, c->celem);
            if (L.n != 8 || !cert_parse_ref(L.t[1], &E->v) || strcmp(L.t[2], "in") ||
                cert_parse_range(&L, 3, &E->lo, &E->hi) != 8) CERT_BAD("bad elem");
        } else if (!strcmp(k0, "ret")) {
            if (c->has_ret) CERT_BAD("a second ret");
            if (L.n != 7 || strcmp(L.t[1], "in") || cert_parse_range(&L, 2, &c->ret_lo, &c->ret_hi) != 7) CERT_BAD("bad ret");
            c->has_ret = true;
        } else if (!strcmp(k0, "accum")) {
            CertAccum *A = CERT_PUSH(c->acc, c->nacc, c->cacc);
            if (L.n != 18 || !cert_parse_ref(L.t[1], &A->v) || strcmp(L.t[2], "header") ||
                !cert_parse_block(L.t[3], &A->block) || strcmp(L.t[4], "trips") || !cert_int(L.t[5], &A->trips) ||
                strcmp(L.t[6], "delta") || cert_parse_irange(&L, 7, &A->dlo, &A->dhi) != 12 ||
                strcmp(L.t[12], "init") || cert_parse_irange(&L, 13, &A->s0lo, &A->s0hi) != 18) CERT_BAD("bad accum");
        } else if (!strcmp(k0, "callsite")) {
            CertCall *K = CERT_PUSH(c->call, c->ncall, c->ccall);
            if (L.n < 5 || !cert_parse_ref(L.t[1], &K->result) || strcmp(L.t[2], "callee") ||
                strcmp(L.t[4], "bind")) CERT_BAD("bad callsite");
            K->callee = cert_strdup(L.t[3], -1);
            int i = 5;
            while (i < L.n && strcmp(L.t[i], "ret")) {
                int64_t k;
                if (K->nbind >= 16 || i + 1 >= L.n || !cert_int(L.t[i], &k) || strcmp(L.t[i+1], "=")) CERT_BAD("bad binding");
                K->bind_k[K->nbind] = (int)k;
                int j = cert_parse_irange(&L, i + 2, &K->bind_lo[K->nbind], &K->bind_hi[K->nbind]);
                if (j < 0) CERT_BAD("bad binding");
                K->nbind++; i = j;
            }
            if (i + 7 != L.n || strcmp(L.t[i+1], "in") || cert_parse_range(&L, i + 2, &K->lo, &K->hi) != L.n)
                CERT_BAD("bad callsite range");
            pending = K;
        } else CERT_BAD("unknown line");
    }
    if (sp != 0) CERT_BAD("a certificate is not closed");
#undef CERT_BAD
    return head;
}

#endif // LAIN_ANALYSIS_CERTIFICATE_H
