#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Lain ADVERSARIAL ALIASING + ARRAY-COPY FUZZER (Phase 0).
#
# The trust fuzzer generates programs Lain should PROVE SAFE. This one is the
# opposite: it generates programs that stress the two fail-open classes the
# 2026-08-24 memory-safety hunt found — call-site ALIASING of restrict'd params
# (E087) and whole/fixed ARRAY COPY codegen — and checks Lain never miscompiles.
#
# Crucially it compiles each ACCEPTED program at BOTH -O0 and -O3, because a
# restrict-aliasing violation is undefined behaviour the OPTIMISER exploits: it
# is invisible to ASan (no OOB) and only manifests as a -O0≠-O3 divergence. For
# each generated program it flags one of:
#
#   • COMPILER CRASH   — lain dies by signal
#   • BROKEN-C         — lain ACCEPTS but the emitted C is rejected by gcc
#   • ALIAS-MISCOMPILE — lain ACCEPTS, C compiles, but -O0 and -O3 disagree
#                        (a restrict UB it should have rejected via E087)
#   • WRONG-COPY       — an array-copy program's result != its computed oracle
#   • UNSOUND (ASan)   — a sanitizer trap at run time
#
# A clean REJECT (E087/E085/…, exit 1) is correct fail-closed behaviour, not a bug.
#
# Usage: bash fuzz_alias.sh [iterations]   (default 250)
# ─────────────────────────────────────────────────────────────────────────────
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
LAIN="$ROOT/lain"
CC="${CC:-gcc}"
N="${1:-250}"
DEFS="-Dlibc_printf=printf"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1"

cd "$ROOT" || exit 1
[[ -x "$LAIN" ]] || gcc -std=c99 -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src" 2>/dev/null
BUGDIR="$ROOT/fuzz_bugs"; mkdir -p "$BUGDIR"
TDIR="$(mktemp -d)"; trap 'rm -rf "$TDIR"' EXIT

r() { echo $(( RANDOM % ($1 - $2 + 1) + $2 )); }

# ── gen_alias: `vadd(k, a[lo1..lo1+k], a[lo2..lo2+k])`. Overlap iff |lo1-lo2|<k.
#    Overlap+write MUST be rejected (E087); disjoint MUST be accepted and correct.
gen_alias() {
    local n=$(r 12 6) k=$(r 4 2) lo1 lo2 i vals=""
    lo1=$(r $((n-k)) 0); lo2=$(r $((n-k)) 0)
    local -a arr=()
    for ((i=0;i<n;i++)); do arr[i]=$(r 20 1); vals+="${arr[i]}"; ((i<n-1)) && vals+=", "; done
    # oracle for the DISJOINT case: dst[i] += src[i], sequential
    local overlap=0; (( lo1<lo2 ? lo2-lo1<k : lo1-lo2<k )) && overlap=1
    EXPECT=""
    if (( overlap == 0 )); then
        local -a out=("${arr[@]}"); local s
        for ((i=0;i<k;i++)); do out[lo1+i]=$(( out[lo1+i] + arr[lo2+i] )); done
        EXPECT="${out[0]}"
    fi
    OVERLAP=$overlap
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc vadd(k usize, dst i32[k], src i32[k]) {
    var i usize = 0
    while i < k decreasing k - i {
        dst[i] = dst[i] + src[i]
        i += 1
    }
}
proc main() i32 {
    var a i32[$n] = [$vals]
    vadd($k, a[$lo1..$((lo1+k))], a[$lo2..$((lo2+k))])
    libc_printf("%d\n", a[0])
    return 0
}
EOF
}

# ── gen_copy: whole fixed-array copy chain with a deterministic oracle.
gen_copy() {
    local n=$(r 6 3) i va="" vb=""
    local -a A=() B=()
    for ((i=0;i<n;i++)); do A[i]=$(r 30 1); B[i]=$(r 30 1); va+="${A[i]}"; vb+="${B[i]}"; ((i<n-1)) && { va+=", "; vb+=", "; }; done
    local idx=$(r $((n-1)) 0) newv=$(r 99 50)
    # a = b (copy); then mutate b[idx]; a[idx] must still equal B[idx]; return a[idx]+b[idx]
    EXPECT=$(( B[idx] + newv )); OVERLAP=0
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var b i32[$n] = [$vb]
    var a i32[$n] = [$va]
    a = b
    b[$idx] = $newv
    libc_printf("%d\n", a[$idx] + b[$idx])
    return 0
}
EOF
}

GENS=(gen_alias gen_copy)
acc=0 rej=0 crash=0 brokenc=0 miscompile=0 unsound=0 wrongcopy=0
for ((it=0; it<N; it++)); do
    g="${GENS[$((RANDOM % ${#GENS[@]}))]}"
    EXPECT=""; OVERLAP=0
    src="$TDIR/f.ln"; "$g" > "$src"
    "$LAIN" "$src" -o "$TDIR/f.c" >/dev/null 2>"$TDIR/lain.err"; rc=$?
    if (( rc >= 128 )); then crash=$((crash+1)); cp "$src" "$BUGDIR/acrash_${it}.ln"; echo "CRASH ($g)"; continue; fi
    if (( rc == 1 )); then rej=$((rej+1)); continue; fi            # fail-closed reject — fine
    if (( rc != 0 )); then crash=$((crash+1)); cp "$src" "$BUGDIR/aexit_${it}.ln"; echo "ODD EXIT ($g)"; continue; fi
    acc=$((acc+1))
    # accepted: an OVERLAPPING alias that Lain accepted is already suspect — the
    # -O0/-O3 differential below will convict it if it actually miscompiles.
    if ! $CC -O0 $DEFS -o "$TDIR/o0" "$TDIR/f.c" 2>"$TDIR/gcc.err"; then
        brokenc=$((brokenc+1)); cp "$src" "$BUGDIR/abrokenc_${it}.ln"; cp "$TDIR/f.c" "$BUGDIR/abrokenc_${it}.c"
        echo "BROKEN-C ($g): $(grep -m1 error: "$TDIR/gcc.err")"; continue
    fi
    $CC -O3 -march=native $DEFS -o "$TDIR/o3" "$TDIR/f.c" 2>/dev/null
    o0="$("$TDIR/o0" 2>/dev/null)"; o3="$("$TDIR/o3" 2>/dev/null)"
    if [[ "$o0" != "$o3" ]]; then
        miscompile=$((miscompile+1)); cp "$src" "$BUGDIR/amiscompile_${it}.ln"
        echo "⚠ ALIAS-MISCOMPILE ($g, overlap=$OVERLAP): -O0=[$o0] -O3=[$o3]"; continue
    fi
    # ASan/UBSan pass
    $CC -fsanitize=address,undefined -fno-sanitize-recover=all -O1 $DEFS -o "$TDIR/san" "$TDIR/f.c" 2>/dev/null && \
       { "$TDIR/san" >/dev/null 2>"$TDIR/san.err" || { grep -qE "runtime error|Sanitizer" "$TDIR/san.err" && { unsound=$((unsound+1)); cp "$src" "$BUGDIR/aunsound_${it}.ln"; echo "⚠ UNSOUND ($g): $(grep -m1 -E 'runtime error' "$TDIR/san.err")"; continue; }; }; }
    # oracle (only set for disjoint-alias and copy)
    if [[ -n "$EXPECT" && "$o0" != "$EXPECT" ]]; then
        wrongcopy=$((wrongcopy+1)); cp "$src" "$BUGDIR/awrong_${it}.ln"
        echo "⚠ WRONG-RESULT ($g): expected [$EXPECT] got [$o0]"; continue
    fi
done
echo "=============================================================="
echo "ALIAS-FUZZ $N iters:  accepted=$acc  rejected(fail-closed)=$rej"
echo "  bugs:  crashes=$crash  broken-C=$brokenc  ALIAS-MISCOMPILE=$miscompile  UNSOUND=$unsound  wrong-result=$wrongcopy"
echo "=============================================================="
(( crash + brokenc + miscompile + unsound + wrongcopy > 0 )) && { echo "repros in fuzz_bugs/"; exit 1; }
exit 0
