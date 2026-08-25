#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Lain STRING-SAFETY FUZZER (Phase 0).
#
# Motivated by a real near-miss: the sentinel-string refactor briefly accepted an
# out-of-bounds `s[3]` after `s = "xy"` (array_len diverged from the value). That
# OOB was caught by HAND under ASan, not by any harness — the fuzzers never
# exercised string indexing / reassignment / `u8[:0]` coercion under a sanitizer.
# This closes that gap: it hammers exactly those paths and would convict such a
# regression automatically.
#
# For each generated program:
#   • a clean REJECT (E085 bounds, E090 length-mismatch, …) is correct fail-closed
#   • ACCEPTED  → compiled under ASan/UBSan and RUN; a trap is an UNSOUND accept
#                 (an OOB read / overread the type system let through)
#   • ACCEPTED with a computable oracle → the output must match (miscompile guard)
#
# The string content is ASCII A–Z only (no escapes / NULs), so a printed `%s` of a
# correctly-terminated `.data` is exactly the content — any trailing garbage is an
# overread.
#
# Usage: bash fuzz_strings.sh [iterations]   (default 300)
# ─────────────────────────────────────────────────────────────────────────────
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
LAIN="$ROOT/lain"
CC="${CC:-gcc}"
N="${1:-300}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -O1"
DEFS="-Dlibc_printf=printf"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1"

cd "$ROOT" || exit 1
[[ -x "$LAIN" ]] || gcc -std=c99 -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src" 2>/dev/null
BUGDIR="$ROOT/fuzz_bugs"; mkdir -p "$BUGDIR"
TDIR="$(mktemp -d)"; trap 'rm -rf "$TDIR"' EXIT

r() { echo $(( RANDOM % ($1 - $2 + 1) + $2 )); }
# a random A–Z string of length $1 (echoed) — no escapes, no NUL
randstr() { local n=$1 s="" i; for ((i=0;i<n;i++)); do s+=$(printf "\\$(printf '%03o' $(r 90 65))"); done; echo "$s"; }
ord() { printf '%d' "'$1"; }

# ── gen_index: index a string literal at a random (maybe OOB) position ──────────
gen_index() {
    local len=$(r 8 1); local k=$(r 11 0); local content=$(randstr $len)
    EXPECT=""; (( k < len )) && EXPECT=$(ord "${content:$k:1}")
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var s = "$content"
    libc_printf("%d\n", s[$k] as i32)
    return 0
}
EOF
}

# ── gen_reassign: reassign then index — different length must be rejected (E090),
#    else array_len diverges from the value and s[k] can read past it ────────────
gen_reassign() {
    local la=$(r 6 1); local lb=$(r 6 1); local a=$(randstr $la); local b=$(randstr $lb); local k=$(r 7 0)
    EXPECT=""; (( la == lb && k < lb )) && EXPECT=$(ord "${b:$k:1}")
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var s = "$a"
    s = "$b"
    libc_printf("%d\n", s[$k] as i32)
    return 0
}
EOF
}

# ── gen_coerce: pass a string to a u8[:0] param, print .data via C %s. Overread
#    if .data is not NUL-terminated. Oracle = the exact content. ─────────────────
gen_coerce() {
    local len=$(r 9 1); local content=$(randstr $len)
    EXPECT="[$content]"
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc show(m u8[:0]) {
    libc_printf("[%s]", m.data)
}
proc main() i32 {
    var s = "$content"
    show(s)
    return 0
}
EOF
}

# ── gen_subslice: sub-slice a string then index it ──────────────────────────────
gen_subslice() {
    local len=$(r 10 3); local lo=$(r 3 0); local hi=$(r "$len" 1); local k
    (( hi <= lo )) && hi=$((lo+1)); (( hi > len )) && hi=$len
    k=$(r 6 0)
    local content=$(randstr $len)
    EXPECT=""; local sublen=$((hi-lo)); (( k < sublen )) && EXPECT=$(ord "${content:$((lo+k)):1}")
    cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc main() i32 {
    var s = "$content"
    var sub = s[$lo..$hi]
    libc_printf("%d\n", sub[$k] as i32)
    return 0
}
EOF
}

GENS=(gen_index gen_reassign gen_coerce gen_subslice)
acc=0 rej=0 crash=0 brokenc=0 unsound=0 wrong=0
for ((it=0; it<N; it++)); do
    g="${GENS[$((RANDOM % ${#GENS[@]}))]}"
    EXPECT=""
    src="$TDIR/s.ln"; "$g" > "$src"
    "$LAIN" "$src" -o "$TDIR/s.c" >/dev/null 2>"$TDIR/lain.err"; rc=$?
    if (( rc >= 128 )); then crash=$((crash+1)); cp "$src" "$BUGDIR/scrash_${it}.ln"; echo "CRASH ($g)"; continue; fi
    if (( rc == 1 )); then rej=$((rej+1)); continue; fi      # fail-closed reject — fine
    if (( rc != 0 )); then crash=$((crash+1)); cp "$src" "$BUGDIR/sexit_${it}.ln"; echo "ODD EXIT ($g)"; continue; fi
    acc=$((acc+1))
    if ! $CC $SAN $DEFS -o "$TDIR/s" "$TDIR/s.c" 2>"$TDIR/gcc.err"; then
        brokenc=$((brokenc+1)); cp "$src" "$BUGDIR/sbrokenc_${it}.ln"; cp "$TDIR/s.c" "$BUGDIR/sbrokenc_${it}.c"
        echo "BROKEN-C ($g): $(grep -m1 error: "$TDIR/gcc.err")"; continue
    fi
    out="$("$TDIR/s" 2>"$TDIR/san.err")"; prc=$?
    if grep -qE "runtime error|Sanitizer|overflow|SUMMARY:" "$TDIR/san.err" || (( prc >= 128 )); then
        unsound=$((unsound+1)); cp "$src" "$BUGDIR/sunsound_${it}.ln"
        echo "⚠ UNSOUND ($g): $(grep -m1 -E 'overflow|runtime error|ERROR' "$TDIR/san.err")"; continue
    fi
    if [[ -n "$EXPECT" && "$out" != "$EXPECT" ]]; then
        wrong=$((wrong+1)); cp "$src" "$BUGDIR/swrong_${it}.ln"
        echo "⚠ WRONG ($g): expected [$EXPECT] got [$out]"; continue
    fi
done
echo "=============================================================="
echo "STRING-FUZZ $N iters:  accepted=$acc  rejected(fail-closed)=$rej"
echo "  bugs:  crashes=$crash  broken-C=$brokenc  UNSOUND=$unsound  wrong-result=$wrong"
echo "=============================================================="
(( crash + brokenc + unsound + wrong > 0 )) && { echo "repros in fuzz_bugs/"; exit 1; }
exit 0
