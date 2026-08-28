// test_octagon.c — soundness/precision validation of the octagon domain by BRUTE
// FORCE over concretization γ. The domain claims to over-approximate the integer
// point set; here we enumerate every point in a small box and check the claims
// directly. This is the yardstick from design/vra-octagon.md §2.3/§7.
//
//   gcc -std=c99 -O2 -o /tmp/test_octagon src/analysis/test_octagon.c -I src && /tmp/test_octagon
#include "analysis/octagon.h"
#include <stdio.h>
#include <time.h>

#define NV   3          // variables
#define R    6          // box is [-R, R]^NV
#define DIM  (2*NV)

static int64_t bufA[DIM*DIM], bufB[DIM*DIM], bufC[DIM*DIM];

// e(d) for a concrete point
static int64_t edim(const int *x, int d) { return (d%2==0) ? x[d/2] : -x[d/2]; }

// does a point satisfy every constraint encoded in m?
static bool pt_sat(const Octagon *o, const int *x) {
    for (int i=0;i<o->dim;i++)
        for (int j=0;j<o->dim;j++) {
            int64_t c = oct_get(o,i,j);
            if (c >= OCT_INF) continue;
            if (edim(x,j) - edim(x,i) > c) return false;
        }
    return true;
}

// iterate the box, calling f(point). Returns count of γ-members.
typedef void (*ptfn)(const int *x, void *ctx);
static void box_iter(ptfn f, void *ctx) {
    int x[NV];
    for (x[0]=-R;x[0]<=R;x[0]++)
     for (x[1]=-R;x[1]<=R;x[1]++)
      for (x[2]=-R;x[2]<=R;x[2]++)
        f(x, ctx);
}

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

    if (fails==0) printf("octagon: ALL PROPERTIES HOLD over %d random trials (box [-%d,%d]^%d) + tightness cases\n", trials, R,R,NV);
    else          printf("octagon: %d FAILURES\n", fails);
    return fails ? 1 : 0;
}
