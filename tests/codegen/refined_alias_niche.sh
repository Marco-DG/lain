#!/usr/bin/env bash
# A refinement alias as a type argument keeps its refinement, and so its niche: Option(Small) and
# Option(Below) (`u8 < 200`, `u8 <= 199`) are ONE instance packed into a u8, its None in the free
# 200..255; Option(u8) has no spare value and carries a tag; an excluded value (`!= 7`) makes a
# separate instance. Read from --dump-niche, beside the trust tests that round-trip the values.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1
fail=0
d1=$(./lain tests/trust/generic_refined_alias_pass.ln --dump-niche 2>&1)
d2=$(./lain tests/trust/generic_refined_alias_order_pass.ln --dump-niche 2>&1)
want() { echo "$1" | grep -qF -- "$2" || { echo "missing: $2"; fail=1; }; }
want "$d1" "[niche] enum 'Option_u8_r0_199': variants=2 payload=1 empty=1 -> packed into u8"
want "$d1" "[niche] enum 'Option_u8': variants=2 payload=1 empty=1 -> uint8_t tag + payload union"
want "$d2" "[niche] enum 'Option_u8_r0_199_ne7': variants=2 payload=1 empty=1 -> packed into u8"
want "$d2" "[niche] enum 'Option_u8_r0_199_ne9': variants=2 payload=1 empty=1 -> packed into u8"
n=$(echo "$d1" | grep -c "^\[niche\] enum 'Option_u8_r")
[ "$n" -eq 1 ] || { echo "Small and Below made $n instances, not one"; fail=1; }
exit $fail
