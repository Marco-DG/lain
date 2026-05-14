#!/usr/bin/env bash
# D-Niche M7: prove that niche-optimized enums emit as a typedef of
# their payload type (single pointer, no struct wrapper, no tag byte).
# We grep the emitted C for the typedef and verify it is NOT a struct.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
tmp_ln="$ROOT/_niche_sizeof_scratch.ln"
tmp_c=$(mktemp --suffix=.c)
trap "rm -f $tmp_ln $tmp_c" EXIT

cat > "$tmp_ln" <<'EOF'
type OptionPtr {
    Some { p *u8 }
    None
}

type ResultPtr {
    Ok { v *u8 }
    Err1
    Err2
    Err3
}

proc main() i32 {
    return 0
}
EOF

cd "$ROOT" || exit 1
"$LAIN" "_niche_sizeof_scratch.ln" -o "$tmp_c" >/dev/null 2>&1 || { echo "lain compile failed"; exit 1; }

ok=1

# OptionPtr should be a pointer-typedef, NOT a struct.
opt_def=$(grep -E '^typedef .* _niche_sizeof_scratch_OptionPtr;' "$tmp_c" | head -1)
if [[ -z "$opt_def" ]]; then
    echo "FAIL: OptionPtr typedef not found"; ok=0
elif echo "$opt_def" | grep -q 'struct'; then
    echo "FAIL: OptionPtr emitted as struct, expected pointer typedef: $opt_def"; ok=0
elif ! echo "$opt_def" | grep -q '\*'; then
    echo "FAIL: OptionPtr typedef has no pointer: $opt_def"; ok=0
fi

# ResultPtr should also be pointer-typedef (multi-empty cascade).
res_def=$(grep -E '^typedef .* _niche_sizeof_scratch_ResultPtr;' "$tmp_c" | head -1)
if [[ -z "$res_def" ]]; then
    echo "FAIL: ResultPtr typedef not found"; ok=0
elif echo "$res_def" | grep -q 'struct'; then
    echo "FAIL: ResultPtr emitted as struct: $res_def"; ok=0
elif ! echo "$res_def" | grep -q '\*'; then
    echo "FAIL: ResultPtr typedef has no pointer: $res_def"; ok=0
fi

# No tag enum should be emitted for these niche-optimized enums.
if grep -qE '_OptionPtr_Tag|_ResultPtr_Tag' "$tmp_c"; then
    echo "FAIL: tag enum emitted for a niche-optimized type"; ok=0
fi

# None / Err1 / Err2 / Err3 constructors must use scalar return,
# not a (Type){.tag=...} struct literal.
if grep -qE '_OptionPtr_None.*{.*tag' "$tmp_c"; then
    echo "FAIL: None constructor uses tag struct literal"; ok=0
fi

if [[ $ok -eq 1 ]]; then
    echo "OK: OptionPtr=$opt_def"
    echo "OK: ResultPtr=$res_def"
    exit 0
fi
exit 1
