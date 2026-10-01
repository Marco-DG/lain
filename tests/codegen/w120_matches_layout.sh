#!/usr/bin/env bash
# W120 fires exactly when the emitted C gives a sum a tag. The warning was decided by the front end
# (sema/niche.h) and the tag by ir/layout.h, and they disagreed: `Result(*T, Err)` (two payloads)
# got a tag in silence, a two-field payload got one in silence, and a sum that needed no tag got
# one with "0 empty variant(s) require 0". Each program below counts W120 lines and tagged
# structs in its C; the two numbers must be equal. --dump-niche must not print a layout the C
# does not have (it printed multi-payload sentinels that never shipped).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/a.ln" <<'LN'
type FileErr { NotFound, Permission, Oom }
type R { Ok { p *i32 }, Err { e FileErr } }
type MaybeS { Some { s u8[] }, Nothing }
type MaybeP { Some { p *i32 }, Nothing }
type Two { Some { p *i32, n i32 }, Nothing }
type Only { One { x i32 } }
type Small = u8 < 200
type MaybeSmall { Some { v Small }, Nothing }
func f() R { return R.Err(FileErr.Oom) }
func g(s u8[]) MaybeS { return MaybeS.Some(s) }
func h() MaybeP { return MaybeP.Nothing }
func t() Two { return Two.Nothing }
func o() Only { return Only.One(1) }
func m() MaybeSmall { return MaybeSmall.Nothing }
func main() i32 { return 0 }
LN
out=$( "$LAIN" "$D/a.ln" -o "$D/a.c" 2>&1 ) || { echo "lain refused a.ln"; echo "$out"; exit 1; }
w=$(echo "$out" | grep -c '^\[W120\]'); tags=$(grep -c 'int32_t tag' "$D/a.c")
[ "$w" -eq "$tags" ] || { echo "W120 x$w but $tags tagged structs"; exit 1; }
[ "$tags" -eq 3 ] || { echo "expected R, MaybeS and Two tagged, got $tags"; exit 1; }
for s in R MaybeS Two; do echo "$out" | grep -q "^\[W120\] Warning: enum '$s'" || { echo "no W120 for $s"; exit 1; }; done
dump=$( "$LAIN" --dump-niche "$D/a.ln" -o "$D/a.c" 2>&1 )
echo "$dump" | grep -q "enum 'R': .*int32_t tag" || { echo "--dump-niche: R is not reported tagged"; exit 1; }
echo "$dump" | grep -q "enum 'MaybeP': .*packed into \*i32" || { echo "--dump-niche: MaybeP is not reported packed"; exit 1; }
exit 0
