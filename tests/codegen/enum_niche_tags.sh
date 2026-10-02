#!/usr/bin/env bash
# A plain enum that declares its width is a niche source (tests/trust/enum_niche_pass.ln), so a
# sum over it is the enum itself, with no tag; one without a declared width is not, and its sum
# keeps a tag. Counted in the emitted C, not inferred from behaviour, which is the same
# either way: exactly one tag (OptP's), and Opt is `typedef K Opt`.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/m.ln" <<'LN'
type K u8 {
    A
    B
}
type P {
    X
    Y
}
type Opt {
    Some { k K }
    None
}
type OptP {
    Some { p P }
    None
}
func a(o Opt) i32 {
    case o {
        Some(k): return 1
        None: return 0
    }
}
func b(o OptP) i32 {
    case o {
        Some(p): return 1
        None: return 0
    }
}
func main() i32 {
    if a(Opt.None) != 0 { return 1 }
    if b(OptP.None) != 0 { return 2 }
    return 0
}
LN
"$LAIN" "$D/m.ln" -o "$D/m.c" > "$D/out" 2>&1 || { echo "lain refused the program"; cat "$D/out"; exit 1; }
tags=$(grep -cE 'int[0-9]+_t tag;' "$D/m.c")
[ "$tags" -eq 1 ] || { echo "expected exactly 1 tag (OptP's), found $tags"; grep -nE 'int[0-9]+_t tag;' "$D/m.c"; exit 1; }
grep -qE 'struct OptP \{ u?int[0-9]+_t tag;' "$D/m.c" || { echo "OptP lost its tag"; exit 1; }
grep -q '^typedef K Opt;' "$D/m.c" || { echo "Opt is not the enum itself"; grep -n 'Opt' "$D/m.c" | head -3; exit 1; }
grep -q "declare the width of 'P'" "$D/out" || { echo "W120 does not say how to drop OptP's tag"; cat "$D/out"; exit 1; }
exit 0
