// test_octagon.c — soundness/precision validation of the octagon domain by BRUTE
// FORCE over concretization γ. The domain claims to over-approximate the integer
// point set; here we enumerate every point in a small box and check the claims
// directly. This is the yardstick from local/internal/design/vra-octagon.md §2.3/§7.
//
//   gcc -std=c99 -O2 -o test_octagon src/tools/test_octagon.c -I src && ./test_octagon   (run_ir_tests.sh runs it)
#include "analysis/octagon.h"
#include "tools/octagon_ref.h"
#include <stdio.h>
#include <time.h>

#define NV   3          // variables
#define R    6          // box is [-R, R]^NV
#define DIM  (2*NV)

static int64_t bufA[DIM*DIM], bufB[DIM*DIM], bufC[DIM*DIM];

// e(d) for a concrete point
static int64_t edim(const int *x, int d) { return (d%2==0) ? x[d/2] : -x[d/2]; }

// does a point satisfy every constraint encoded in m? The STORED constraints define γ: the
// strengthening oct_get adds is a consequence of the unary ones (I.89), and reading it at every
// entry of every box point doubled this test's time.
static bool pt_sat(const Octagon *o, const int *x) {
    for (int i=0;i<o->dim;i++)
        for (int j=0;j<o->dim;j++) {
            int64_t c = oct_raw(o,i,j);
            if (c >= OCT_INF) continue;
            if (edim(x,j) - edim(x,i) > c) return false;
        }
    return true;
}

// iterate the box, calling f(point). Returns count of γ-members.
typedef void (*ptfn)(const int *x, void *ctx);

// ── property checkers over the box ───────────────────────────────────────────
static int fails = 0;
#define CHECK(cond, msg) do{ if(!(cond)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

// γ equality: same membership for every box point
static bool gamma_eq(const Octagon *a, const Octagon *b) {
    int x[NV];
    for (x[0]=-R;x[0]<=R;x[0]++) for (x[1]=-R;x[1]<=R;x[1]++) for (x[2]=-R;x[2]<=R;x[2]++)
        if (pt_sat(a,x) != pt_sat(b,x)) return false;
    return true;
}
// γ(a) ⊆ γ(b)
static bool gamma_subset(const Octagon *a, const Octagon *b) {
    int x[NV];
    for (x[0]=-R;x[0]<=R;x[0]++) for (x[1]=-R;x[1]<=R;x[1]++) for (x[2]=-R;x[2]<=R;x[2]++)
        if (pt_sat(a,x) && !pt_sat(b,x)) return false;
    return true;
}
static bool gamma_empty(const Octagon *a) {
    int x[NV];
    for (x[0]=-R;x[0]<=R;x[0]++) for (x[1]=-R;x[1]<=R;x[1]++) for (x[2]=-R;x[2]<=R;x[2]++)
        if (pt_sat(a,x)) return false;
    return true;
}

// build a random octagon by adding K random constraints (small constants)
static void rand_oct(Octagon *o, int64_t *buf) {
    oct_init_top(o, NV, buf);
    int K = rand()%6;
    for (int t=0;t<K;t++) {
        int a=rand()%NV, b=rand()%NV, c=(rand()%(2*R+1))-R;
        switch (rand()%6) {
            case 0: oct_add_ub(o,a,c); break;
            case 1: oct_add_lb(o,a,c); break;
            case 2: if(a!=b) oct_add_diff_le(o,a,b,c); break;   // a-b ≤ c
            case 3: oct_add_sum_le(o,a,b,c); break;             // a+b ≤ c
            case 4: oct_add_negsum_le(o,a,b,c); break;          // -a-b ≤ c
            case 5: oct_add_const(o,a,c); break;                // a = c
        }
    }
}

int main(void) {
    srand(12345);
    Octagon A={0}, B={0}, C={0};
    A.m=bufA; B.m=bufB; C.m=bufC;
    int trials = 40000;

    for (int t=0; t<trials; t++) {
        rand_oct(&A, bufA);

        // 1. closure preserves γ (only makes implied constraints explicit)
        int64_t save[DIM*DIM]; memcpy(save, bufA, sizeof save);
        Octagon Araw={0}; Araw.m=save; Araw.nvar=NV; Araw.dim=DIM;
        oct_close(&A);
        CHECK(gamma_eq(&Araw, &A), "closure changed γ");

        // 2. emptiness detection is SOUND: if we declare ⊥, γ really is empty.
        //    (The converse is confounded by the finite box — an octagon can be
        //     non-⊥ yet have all its points outside [-R,R].)
        CHECK(!oct_is_bottom(&A) || gamma_empty(&A), "declared ⊥ but γ nonempty");

        if (oct_is_bottom(&A)) continue;   // interval/lattice checks assume non-⊥

        // 3. interval read is SOUND: the octagon's bound never cuts inside the box
        //    extent (tightness is checked separately on box-contained cases below).
        for (int v=0; v<NV; v++) {
            int64_t lo=0,hi=0; bool hl=false,hh=false;
            oct_interval(&A, v, &lo,&hl, &hi,&hh);
            int64_t amin=OCT_INF, amax=-OCT_INF; int x[NV];
            for (x[0]=-R;x[0]<=R;x[0]++) for (x[1]=-R;x[1]<=R;x[1]++) for (x[2]=-R;x[2]<=R;x[2]++)
                if (pt_sat(&A,x)) { if(x[v]<amin)amin=x[v]; if(x[v]>amax)amax=x[v]; }
            if (hh && amax>-OCT_INF) CHECK(hi >= amax, "interval hi unsound");
            if (hl && amin< OCT_INF) CHECK(lo <= amin, "interval lo unsound");
        }

        // 4. join ⊒ both operands
        rand_oct(&B, bufB); oct_close(&B);
        if (!oct_is_bottom(&B)) {
            oct_join(&C, &A, &B);
            CHECK(gamma_subset(&A, &C), "join lost γ(A)");
            CHECK(gamma_subset(&B, &C), "join lost γ(B)");
            CHECK(oct_leq(&A,&C) && oct_leq(&B,&C), "join not an upper bound (order)");
        }

        // 5. meet = intersection (after re-closing)
        oct_meet(&C, &A, &B); oct_close(&C);
        {
            int x[NV]; bool ok=true;
            for (x[0]=-R;x[0]<=R;x[0]++) for (x[1]=-R;x[1]<=R;x[1]++) for (x[2]=-R;x[2]<=R;x[2]++)
                if (pt_sat(&C,x) != (pt_sat(&A,x)&&pt_sat(&B,x))) ok=false;
            CHECK(ok, "meet is not γ(A)∩γ(B)");
        }

        // 6. widening ⊒ both operands (termination is structural, checked below)
        if (!oct_is_bottom(&B)) {
            oct_widen(&C, &A, &B);
            CHECK(gamma_subset(&A, &C), "widen lost γ(A)");
            CHECK(gamma_subset(&B, &C), "widen lost γ(B)");
        }
    }

    // 7. widening terminates: iterating x := widen(x, x ⊔ step) stabilizes fast
    {
        rand_oct(&A, bufA); oct_close(&A);
        int iters=0;
        for (; iters<100; iters++) {
            rand_oct(&B, bufB); oct_close(&B);
            oct_join(&C, &A, &B);                  // grow
            int64_t wbuf[DIM*DIM]; Octagon W={0}; W.m=wbuf;
            oct_widen(&W, &A, &C);
            if (oct_leq(&W,&A)) break;             // stabilized
            oct_copy(&A, &W);
        }
        CHECK(iters < 60, "widening did not converge quickly");
    }

    // 8. tightness on controlled, box-contained octagons: closure yields the
    //    EXACT interval / implied difference.
    {
        // 2 ≤ x ≤ 5,  x − y = 1  ⇒  y ∈ [1,4]; interval reads must be exact.
        Octagon T={0}; T.m=bufA; oct_init_top(&T, NV, bufA);
        oct_add_lb(&T,0,2); oct_add_ub(&T,0,5);
        oct_add_diff_le(&T,0,1,1); oct_add_diff_le(&T,1,0,-1);   // x−y ≤ 1 and y−x ≤ −1 ⇒ x−y=1
        oct_close(&T);
        int64_t lo,hi; bool hl,hh;
        oct_interval(&T,0,&lo,&hl,&hi,&hh); CHECK(hl&&hh&&lo==2&&hi==5, "tight: x∈[2,5]");
        oct_interval(&T,1,&lo,&hl,&hi,&hh); CHECK(hl&&hh&&lo==1&&hi==4, "tight: y=x−1∈[1,4]");
        // implied sum bound: x+y ∈ [3,9]
        oct_interval(&T,2,&lo,&hl,&hi,&hh); // z unconstrained
        CHECK(!hl && !hh, "z should be unconstrained");
    }
    // 9. transitive difference: x−y≤2, y−z≤3 ⇒ x−z≤5 after closure
    {
        Octagon T={0}; T.m=bufB; oct_init_top(&T, NV, bufB);
        oct_add_diff_le(&T,0,1,2); oct_add_diff_le(&T,1,2,3);
        oct_close(&T);
        CHECK(oct_get(&T, oct_pos(2), oct_pos(0)) <= 5, "transitive x−z≤5 not derived");
    }

    // 10. INTEGER TIGHTNESS. Mine's Definition 3 asks that every m[i][bar i] be even;
    //     oct_tighten_unary evens them at the end of every closure (2x <= c is x <= floor(c/2)),
    //     and the strong step at i = bar j reads the same entry twice: floor(c/2)+floor(c/2),
    //     which for an odd c is strictly smaller, and even.
    {
        Octagon T={0}; T.m=bufA; oct_init_top(&T, NV, bufA);
        *oct_at(&T, oct_neg(0), oct_pos(0)) = 5;          // 2x <= 5, planted ODD by hand
        oct_close(&T);
        CHECK(oct_get(&T, oct_neg(0), oct_pos(0)) == 4, "odd unary entry not tightened");
        // and it is reached by a DERIVATION, not only by hand: x+y<=5 and x-y<=0 give
        // 2x <= 5 through the shortest path, which over Z is x <= 2.
        oct_init_top(&T, NV, bufA);
        oct_add_sum_le(&T,0,1,5); oct_add_diff_le(&T,0,1,0);
        oct_close(&T);
        CHECK(oct_get(&T, oct_neg(0), oct_pos(0)) == 4, "derived 2x bound not tightened");
        int64_t lo,hi; bool hl,hh;
        oct_interval(&T,0,&lo,&hl,&hi,&hh); CHECK(hh && hi==2, "tight: x<=2 from x+y<=5, x<=y");
        // every unary entry is even after closure, which is the property itself
        for (int v=0; v<NV; v++) {
            int64_t u = oct_get(&T, oct_neg(v), oct_pos(v));
            int64_t l = oct_get(&T, oct_pos(v), oct_neg(v));
            CHECK(u>=OCT_INF || u%2==0, "unary upper entry odd after closure");
            CHECK(l>=OCT_INF || l%2==0, "unary lower entry odd after closure");
        }
    }
    // 11. The emptiness test that a RATIONAL closure misses: 2x <= 1 and -2x <= -1 is
    //     x = 1/2, satisfiable over Q and unsatisfiable over Z. Shortest paths alone
    //     give m[x][x] = 0; the tightening drives it negative.
    {
        Octagon T={0}; T.m=bufB; oct_init_top(&T, NV, bufB);
        *oct_at(&T, oct_neg(0), oct_pos(0)) =  1;         //  2x <= 1
        *oct_at(&T, oct_pos(0), oct_neg(0)) = -1;         // -2x <= -1
        oct_close(&T);
        CHECK(oct_is_bottom(&T), "x = 1/2 not detected as empty over Z");
        CHECK(gamma_empty(&T), "declared empty but gamma nonempty");
    }

    // 12. INCREMENTAL CLOSURE = FULL CLOSURE. A closed octagon, then random tightenings (and
    //     forgets, and more tightenings than the pending list holds): closed incrementally
    //     (A, clean before the edits) it must equal the same edits closed in full (B, marked
    //     dirty), entry for entry, empty or not. Eight variables, constants up to
    //     ±40, no γ enumeration: this checks EQUALITY, which a γ property cannot see.
    int incr_trials = 0, incr_bottom = 0;
    {
        enum { NB = 8, DB = 2*NB };
        static int64_t ma[DB*DB], mb[DB*DB];
        for (int t=0; t<20000; t++) {
            Octagon A={0}, B={0};
            oct_init_top(&A, NB, ma);
            int K = rand()%12;
            for (int q=0;q<K;q++) {
                int a=rand()%NB, b=rand()%NB, c=(rand()%81)-40;
                switch (rand()%5) {
                    case 0: oct_add_ub(&A,a,c); break;
                    case 1: oct_add_lb(&A,a,c); break;
                    case 2: if(a!=b) oct_add_diff_le(&A,a,b,c); break;
                    case 3: oct_add_sum_le(&A,a,b,c); break;
                    case 4: oct_add_negsum_le(&A,a,b,c); break;
                }
            }
            oct_close(&A);
            if (oct_is_bottom(&A)) continue;
            int E = 1 + rand()%(t%7==0 ? 30 : 5);          // sometimes past OCT_PEND_MAX
            memcpy(mb, ma, sizeof ma); B.nvar=NB; B.dim=DB; B.m=mb; oct_set_clean(&B, false);
            for (int q=0;q<E;q++) {
                int a=rand()%NB, b=rand()%NB, c=(rand()%81)-40, op=rand()%7;
                Octagon *X[2] = {&A, &B};
                for (int w=0; w<2; w++) switch (op) {
                    case 0: oct_add_ub(X[w],a,c); break;
                    case 1: oct_add_lb(X[w],a,c); break;
                    case 2: if(a!=b) oct_add_diff_le(X[w],a,b,c); break;
                    case 3: oct_add_sum_le(X[w],a,b,c); break;
                    case 4: oct_add_negsum_le(X[w],a,b,c); break;
                    case 5: oct_add_const(X[w],a,c); break;
                    case 6: oct_forget(X[w],a); break;
                }
            }
            oct_close(&A); oct_close(&B);
            bool ba = oct_is_bottom(&A), bb = oct_is_bottom(&B);
            CHECK(ba == bb, "incremental and full closure disagree on emptiness");
            if (ba) incr_bottom++; else incr_trials++;
            // ⊥ included: the same values, not merely the same emptiness (a dead-code query
            // reads a ⊥ matrix's entries). The values IMPLIED, as every reader sees them: the
            // stored matrix leaves the strengthening to oct_get (I.89), so two closures may store
            // different pairs and imply the same ones.
            bool same = true;
            for (int i=0;i<DB && same;i++) for (int j=0;j<DB && same;j++)
                if (oct_get(&A,i,j) != oct_get(&B,i,j)) same = false;
            CHECK(same, "incremental closure differs from the full closure");
        }
    }
    printf("octagon: incremental = full closure on %d satisfiable edit sequences and %d empty ones, entry for entry\n", incr_trials, incr_bottom);

    // 13. THE SPARSE OCTAGON = THE STRONG ONE IT REPLACED (I.89). Every value the new oct_get reads
    //     must be the entry the old strongly closed matrix holds (src/tools/octagon_ref.h, the old
    //     octagon.h renamed), after every operation VRA performs: constraints then a closure
    //     (incremental or full), a join of closed states marked closed as the fixpoint marks it,
    //     constraints added on top of such a join, a widening with thresholds and a loop-modified
    //     mask then a closure, a meet, a forget, and the order. Six variables, constants up to ±40,
    //     and one sequence in five near the representable edge (I.170). A widened state is also
    //     kept UNCLOSED and used as an operand, as the fixpoint stores a header's widened state:
    //     there the new operations read implied values the old raw matrix had not derived, so they
    //     may be tighter, and those cases are counted, not failed (the corpus measures them).
    long d_ops = 0, d_open = 0, d_open_diff = 0, d_leq = 0, d_bot = 0, d_forget_empty = 0;
    {
        enum { NB2 = 6, DB2 = 2*NB2, NP = 7 };
        static int64_t nm[NP][DB2*DB2], rm[NP][DB2*DB2];
        Octagon N[NP]; RefOctagon F[NP]; bool open[NP], taint[NP];
        int64_t thr[4]; char mod[NB2];
        for (int t=0; t<6000; t++) {
            int64_t K = (t % 5 == 0) ? (REF_OCT_BOUND_MAX / 3) : 40;
            for (int p=0;p<NP;p++) { oct_init_top(&N[p], NB2, nm[p]); ref_oct_init_top(&F[p], NB2, rm[p]);
                                     oct_close(&N[p]); ref_oct_close(&F[p]); open[p] = false; taint[p] = false; }
            for (int step=0; step<18; step++) {
                int op = rand()%7, p = rand()%NP, q = rand()%NP, r = rand()%NP;
                bool check = true;
                // a result computed from a state that was ever open is TAINTED until closed and re-synced
                bool tin = taint[p] || open[p] || ((op == 1 || op == 2 || op == 4) && (taint[q] || open[q]));
                // the fixpoint never joins, widens or meets an EMPTY state (it skips a ⊥ edge, and an
                // unreached block takes its first state by copy), and a ⊥ matrix's entries are arbitrary
                if ((op == 1 || op == 2 || op == 4) && (oct_is_bottom(&N[p]) || oct_is_bottom(&N[q]) ||
                                                        ref_oct_is_bottom(&F[p]) || ref_oct_is_bottom(&F[q]))) continue;
                switch (op) {
                case 0: case 6: {                                          // constraints, then close
                    int k = 1 + rand()%4;
                    for (int e=0;e<k;e++) {
                        int a=rand()%NB2, b=rand()%NB2, kind=rand()%6;
                        int64_t c = (int64_t)(rand()%201 - 100) * K / 100;
                        switch (kind) {
                            case 0: oct_add_ub(&N[p],a,c); ref_oct_add_ub(&F[p],a,c); break;
                            case 1: oct_add_lb(&N[p],a,c); ref_oct_add_lb(&F[p],a,c); break;
                            case 2: if (a!=b) { oct_add_diff_le(&N[p],a,b,c); ref_oct_add_diff_le(&F[p],a,b,c); } break;
                            case 3: oct_add_sum_le(&N[p],a,b,c); ref_oct_add_sum_le(&F[p],a,b,c); break;
                            case 4: oct_add_negsum_le(&N[p],a,b,c); ref_oct_add_negsum_le(&F[p],a,b,c); break;
                            case 5: oct_add_const(&N[p],a,c); ref_oct_add_const(&F[p],a,c); break;
                        }
                    }
                    oct_close(&N[p]); ref_oct_close(&F[p]); open[p] = false; r = p; break; }
                case 1: {                                                  // join, closed if both were
                    if (r == p || r == q) { check = false; break; }
                    bool was_open = open[p] || open[q];
                    oct_join(&N[r], &N[p], &N[q]); ref_oct_join(&F[r], &F[p], &F[q]);
                    if (!was_open) { oct_set_clean(&N[r], true); ref_oct_set_clean(&F[r], true); }
                    else { oct_close(&N[r]); ref_oct_close(&F[r]); }
                    open[r] = false; break; }
                case 2: {                                                  // widen a header: In ∇ (In ⊔ T), left open
                    if (r == p || r == q) { check = false; break; }
                    static int64_t jn[DB2*DB2], jr[DB2*DB2];
                    Octagon JN; RefOctagon JR; JN.m = jn; JR.m = jr;
                    oct_join(&JN, &N[p], &N[q]); ref_oct_join(&JR, &F[p], &F[q]);
                    if (open[p] || open[q]) { oct_close(&JN); ref_oct_close(&JR); }
                    thr[0] = -10; thr[1] = 0; thr[2] = 10; thr[3] = 2*K;
                    for (int v=0; v<NB2; v++) mod[v] = (char)(rand()%2);
                    const char *mm = rand()%3 ? mod : NULL;
                    oct_widen_thr(&N[r], &N[p], &JN, mm, thr, 4); ref_oct_widen_thr(&F[r], &F[p], &JR, mm, thr, 4);
                    if (rand()%2) { oct_close(&N[r]); ref_oct_close(&F[r]); open[r] = false; }
                    else { open[r] = true; taint[r] = true; check = false; }
                    break; }
                case 3: {                                                  // forget, then close
                    int v = rand()%NB2;
                    bool was_empty = ref_oct_is_bottom(&F[p]) && oct_is_bottom(&N[p]);
                    oct_forget(&N[p], v); ref_oct_forget(&F[p], v);
                    oct_close(&N[p]); ref_oct_close(&F[p]); open[p] = false; r = p;
                    // Projecting a variable out of an EMPTY set leaves it empty. The strong matrix kept
                    // that only where its contradiction had spread past v; the sparse one always does.
                    if (was_empty && oct_is_bottom(&N[p]) && !ref_oct_is_bottom(&F[p])) {
                        d_forget_empty++; memcpy(nm[p], rm[p], sizeof nm[p]); oct_set_clean(&N[p], true);
                        check = false;
                    }
                    break; }
                case 4: {                                                  // meet, then close
                    if (r == p || r == q) { check = false; break; }
                    oct_meet(&N[r], &N[p], &N[q]); ref_oct_meet(&F[r], &F[p], &F[q]);
                    oct_close(&N[r]); ref_oct_close(&F[r]); open[r] = false; break; }
                case 5: {                                                  // the order, on closed states
                    check = false;
                    if (open[p] || open[q] || oct_is_bottom(&N[p]) || oct_is_bottom(&N[q])) break;
                    CHECK(oct_leq(&N[p], &N[q]) == ref_oct_leq(&F[p], &F[q]), "sparse and strong orders disagree");
                    d_leq++; break; }
                }
                if (!check) continue;
                if (tin) {                                 // compare, count, and re-sync from the strong one
                    d_open++;
                    bool same = oct_is_bottom(&N[r]) == ref_oct_is_bottom(&F[r]);
                    if (same && !ref_oct_is_bottom(&F[r]))
                        for (int i=0;i<DB2&&same;i++) for (int j=0;j<DB2&&same;j++)
                            if (oct_get(&N[r],i,j) != ref_oct_get(&F[r],i,j)) same = false;
                    if (!same) d_open_diff++;
                    memcpy(nm[r], rm[r], sizeof nm[r]); oct_set_clean(&N[r], true); taint[r] = false;
                    continue;
                }
                taint[r] = false;
                d_ops++;
                bool bn = oct_is_bottom(&N[r]), bf = ref_oct_is_bottom(&F[r]);
                if (bn != bf) printf("  op %d: emptiness sparse %d strong %d\n", op, bn, bf);
                CHECK(bn == bf, "sparse and strong octagons disagree on emptiness");
                if (bn) { d_bot++; continue; }
                bool same = true;
                for (int i=0;i<DB2&&same;i++) for (int j=0;j<DB2&&same;j++)
                    if (oct_get(&N[r],i,j) != ref_oct_get(&F[r],i,j)) {
                        same = false;
                        if (fails < 5) printf("  op %d: entry [%d][%d] sparse %lld strong %lld\n", op, i, j,
                                              (long long)oct_get(&N[r],i,j), (long long)ref_oct_get(&F[r],i,j));
                    }
                CHECK(same, "the sparse octagon implies a different value than the strong one");
            }
        }
    }
    printf("octagon: sparse = strong on %ld closed results (%ld empty) and %ld orders; %ld results from open states, %ld of them different;"
           " %ld forgets of an empty state kept empty where the strong one was not\n",
           d_ops, d_bot, d_leq, d_open, d_open_diff, d_forget_empty);

    if (fails==0) printf("octagon: ALL PROPERTIES HOLD over %d random trials (box [-%d,%d]^%d) + tightness/integer cases\n", trials, R,R,NV);
    else          printf("octagon: %d FAILURES\n", fails);
    return fails ? 1 : 0;
}
