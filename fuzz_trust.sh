#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Lain TRUST FUZZER (Phase 0, generative).
#
# The curated harness tests the cases a human thought of. This generates random
# proof-EXERCISING programs and runs each through the same trust pipeline, to find
# the cases nobody wrote. For every generated program it flags one of:
#
#   • COMPILER CRASH   — lain dies by signal (robustness bug)
#   • BROKEN-C         — lain ACCEPTS, but the emitted C is rejected by the C
#                        compiler (codegen bug)
#   • UNSOUND PROOF    — lain accepts, C compiles, but ASan/UBSan traps at run
#                        time: Lain proved something FALSE (the critical bug)
#
# A clean REJECT (E-code, exit 1) is fine — fail-closed conservatism, not a bug.
# The generators bias toward patterns Lain should PROVE safe (modulo/loop bounds,
# Path-F-widened arithmetic, checked ops, niche unions), with random sizes/values
# that hammer the edges those proofs must get exactly right.
#
# Usage: bash fuzz_trust.sh [iterations]   (default 300)
# Failing programs are saved to fuzz_bugs/ for reproduction.
# ─────────────────────────────────────────────────────────────────────────────
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
LAIN="$ROOT/lain"
CC="${CC:-gcc}"
N="${1:-300}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -O1"
DEFS="-Dlibc_printf=printf -Dlibc_malloc=malloc -Dlibc_free=free"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1"

cd "$ROOT" || exit 1
[[ -x "$LAIN" ]] || gcc -std=c99 -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src" 2>/dev/null
BUGDIR="$ROOT/fuzz_bugs"; rm -rf "$BUGDIR"; mkdir -p "$BUGDIR"
TDIR="$(mktemp -d)"; trap 'rm -rf "$TDIR"' EXIT

r() { echo $(( RANDOM % ($1 - $2 + 1) + $2 )); }          # random in [$2,$1]
pick() { local a=("$@"); echo "${a[$((RANDOM % ${#a[@]}))]}"; }
big() { echo $(( (RANDOM<<15 | RANDOM) * ( (RANDOM%2)?1:-1 ) )); }  # ~wide signed

# ── generators: each echoes a full program that Lain SHOULD prove safe ──────────
gen_modbounds() {   # buf[i % N] in a loop — VRA modulo-bounds proof
    local n=$(r 12 2) steps=$(r 40 1) i vals=""
    for ((i=0;i<n;i++)); do vals+="$(r 90 65)"; ((i<n-1)) && vals+=", "; done
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var buf = [$vals]
    var acc = 0
    var i = 0
    while i < $steps decreasing $steps - i {
        acc = acc + buf[i % $n]
        i += 1
    }
    libc_printf("%d\n", acc)
    return 0
}
EOF
}
gen_scanbounds() {  # forward scan to exact len — off-by-one → ASan OOB
    local n=$(r 16 1) i vals=""
    for ((i=0;i<n;i++)); do vals+="$(r 120 1)"; ((i<n-1)) && vals+=", "; done
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var a = [$vals]
    var s = 0
    var i = 0
    while i < $n decreasing $n - i {
        s = s + a[i]
        i += 1
    }
    libc_printf("%d\n", s)
    return 0
}
EOF
}
gen_widen() {       # Path-F widening: i32*i32 must compute wide (no i32 UB)
    local a=$(big) b=$(big)
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var x i32 = $a
    var y i32 = $b
    var p i64 = x * y
    var q i64 = x + y
    libc_printf("%lld %lld\n", p, q)
    return 0
}
EOF
}
gen_checked() {     # checked op must never UB: recovers on overflow
    local a=$(big) b=$(big) op=$(pick '+?' '-?' '*?')
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var x i32 = $a
    var y i32 = $b
    var z i32 = x $op y else 0
    libc_printf("%d\n", z)
    return 0
}
EOF
}
gen_narrow() {      # narrow-type arithmetic: u8/u16 widening, no promotion UB
    local T=$(pick u8 u16) a=$(r 250 0) b=$(r 250 0) op=$(pick '+' '*' '+%' '+|')
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var x $T = $a
    var y $T = $b
    var z i64 = (x $op y) as i64
    libc_printf("%lld\n", z)
    return 0
}
EOF
}
gen_niche() {       # niche union round-trip must not corrupt the value
    local miss=$(pick true false) v=$(r 120 33)
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
func pick(p *u8, m bool) *u8 | Gone { if m { return Gone } return p }
proc main() i32 {
    var r *u8 | Gone = pick("Z", $miss)
    case r { Gone: libc_printf("gone\n") else: libc_printf("%s\n", r) }
    return 0
}
EOF
}

gen_modmismatch() { # buf[i % M] with M INDEPENDENT of len — is the bounds proof
    local n=$(r 8 2) m=$(r 12 2) steps=$(r 30 1) i vals=""   # SOUND when M can exceed len?
    for ((i=0;i<n;i++)); do vals+="$(r 90 65)"; ((i<n-1)) && vals+=", "; done
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var buf = [$vals]
    var acc = 0
    var i = 0
    while i < $steps decreasing $steps - i {
        acc = acc + buf[i % $m]
        i += 1
    }
    libc_printf("%d\n", acc)
    return 0
}
EOF
}
gen_offset() {      # a[i + K] in a loop — offset-index bounds proof
    local n=$(r 12 3); local k=$(r 4 0); local steps=$(r "$n" 1); local i vals=""
    for ((i=0;i<n;i++)); do vals+="$(r 90 1)"; ((i<n-1)) && vals+=", "; done
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var a = [$vals]
    var s = 0
    var i = 0
    while i + $k < $n decreasing $n - i {
        s = s + a[i + $k]
        i += 1
    }
    libc_printf("%d\n", s)
    return 0
}
EOF
}
gen_subidx() {      # sub-slice then index it — sub-slice bounds/offset soundness
    local n=$(r 12 4); local lo=$(r 3 0); local hi=$(r "$n" 4); local k i vals=""
    (( hi <= lo )) && hi=$((lo+2)); (( hi > n )) && hi=$n
    k=$(r $((hi-lo-1)) 0)
    for ((i=0;i<n;i++)); do vals+="$(r 90 1)"; ((i<n-1)) && vals+=", "; done
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var xs = [$vals]
    var s = xs[$lo..$hi]
    libc_printf("%d\n", s[$k])
    return 0
}
EOF
}
gen_slicefn() {     # slice through a function, indexed via `in` guard
    local len=$(r 6 1) i s=""
    for ((i=0;i<len;i++)); do s+="$(printf '\\x%02x' $(r 90 65))"; done
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
func first(d u8[:0]) i32 {
    if d.len > 0 { return d[0] as i32 }
    return -1
}
proc main() i32 {
    libc_printf("%d\n", first("$s"))
    return 0
}
EOF
}

GENS=(gen_modbounds gen_scanbounds gen_widen gen_checked gen_narrow gen_niche \
      gen_modmismatch gen_offset gen_subidx gen_slicefn)

acc=0 rej=0 crash=0 brokenc=0 unsound=0
for ((it=0; it<N; it++)); do
    g="${GENS[$((RANDOM % ${#GENS[@]}))]}"
    src="$TDIR/f.ln"; "$g" > "$src"
    "$LAIN" "$src" -o "$TDIR/f.c" >/dev/null 2>"$TDIR/lain.err"; rc=$?
    if (( rc >= 128 )); then
        crash=$((crash+1)); cp "$src" "$BUGDIR/crash_${it}.ln"; echo "CRASH ($g, sig $((rc-128)))"; continue
    fi
    if (( rc == 1 )); then rej=$((rej+1)); continue; fi         # clean reject — fine
    if (( rc != 0 )); then crash=$((crash+1)); cp "$src" "$BUGDIR/exit${rc}_${it}.ln"; echo "ODD EXIT $rc ($g)"; continue; fi
    acc=$((acc+1))
    if ! $CC $SAN $DEFS -o "$TDIR/f" "$TDIR/f.c" 2>"$TDIR/gcc.err"; then
        brokenc=$((brokenc+1)); cp "$src" "$BUGDIR/brokenc_${it}.ln"; cp "$TDIR/f.c" "$BUGDIR/brokenc_${it}.c"
        echo "BROKEN-C ($g): $(grep -m1 error: "$TDIR/gcc.err")"; continue
    fi
    "$TDIR/f" >/dev/null 2>"$TDIR/san.err"; prc=$?
    if grep -qE "runtime error|Sanitizer|SUMMARY:" "$TDIR/san.err" || (( prc >= 128 )); then
        unsound=$((unsound+1)); cp "$src" "$BUGDIR/unsound_${it}.ln"
        echo "⚠ UNSOUND PROOF ($g): $(grep -m1 -E 'runtime error|ERROR' "$TDIR/san.err")"; continue
    fi
done

echo "=============================================================="
echo "FUZZ $N iters:  accepted=$acc  rejected=$rej"
echo "  bugs:  crashes=$crash  broken-C=$brokenc  UNSOUND=$unsound"
echo "=============================================================="
(( crash + brokenc + unsound > 0 )) && { echo "repro programs in fuzz_bugs/"; exit 1; }
exit 0
