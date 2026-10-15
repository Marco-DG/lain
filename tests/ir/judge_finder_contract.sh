#!/usr/bin/env bash
# judge_finder_contract.sh — the finder's late templates (regions c38, regions against a fixed value,
# entry-relative claims c40, |v| + |w| growing c41) ask every obligation the judge will owe before
# they propose a ranking, so a proposal they make is PROVED: never a rank.* record not-implied. A
# compile cannot show the difference (a refused proposal and none both read E011), so this runs the
# judge's own driver on loops a late template could rank wrongly, each built to tempt one, and fails
# on any rank.* or rec.* record that is neither proved nor outside (not-implied, budget, capacity,
# refused, solver-bug: a proposal the judge cannot decide breaks the contract too), and on a program
# with no such record at all (a parser that reads nothing must not pass). A program that runs for
# ever must also keep an `outside` one: its tempted loop got no proposal.
# - negkeep: v kept negative on the path that lowers q only where v >= 0 (the template once took the
#   case of every version >= 0 after the trip as the one standing for a kept case, and dropped v's;
#   the Secondary Compiler Worker's probe, its arithmetic unchecked as written: with checked steps
#   the template is never reached, and only rank.* and rec.* records are read here);
# - shrink: |a| grows on one path and shrinks to 0 on the other (the template's keep check). It ends
#   on every input (c > 0 at the overflow panic, else q falls), by <q, C - |a|>, so a finder that
#   ranks it so may: only the template's <min, rest> order is wrong;
# - gap, over, larraz, swap: twins from judge_finder_regions, _regions_fixed, _entry and _absgrow;
# - multr, twowayr, ex5r: recursions from judge_finder_recursion, each passing on what should fall
#   (the regions, the parameter split and the max of affine rows at a call); swapr, 4Nested's swap as
#   a recursion (a recursion of several rows reaches every template; the recursive 4Nested itself is
#   ranked by one row, so it does not reach |v| + |w| growing).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
gcc -std=c99 -O2 -w -o "$T/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "judge_vc_driver does not build"; exit 1; }
cat > "$T/negkeep.ln" <<'LN'
func task(nd0 i32, nd1 i32, nd2 i32) i32 {
    var v i32 = nd0
    var q i32 = nd1
    var c i32 = nd2
    if !((-1000 <= v) and (v <= 1000)) {
        return 0
    }
    while q > 0 {
        if c > 0 {
            v = 3 * v
            q = q - 1
        } else {
            q = (q - v) - 1
        }
    }
    return 0
}
LN
cat > "$T/shrink.ln" <<'LN'
func task(nd0 i32, nd1 i32, nd2 i32) i32 effects raises {
    var a i32 = nd0
    var q i32 = nd1
    var c i32 = nd2
    if !((-1000 <= a) and (a <= 1000)) {
        return 0
    }
    while q > 0 {
        if c > 0 {
            a = (3 *? a else panic("overflow")) +? 1 else panic("overflow")
        } else {
            a = 0
            q = q -? 1 else panic("overflow")
        }
    }
    return 0
}
LN
cat > "$T/gap.ln" <<'LN'
extern func nondet() i32 effects io
func task(x0 i32, y0 i32) effects io, raises {
    var x = x0
    var y = y0
    while (x != 0) and (y > 0) {
        if x > 1 {
            if nondet() != 0 {
                x = x -? 1 else panic("overflow")
                y = nondet()
            } else {
                y = y -? 1 else panic("overflow")
            }
        } else {
            if x < 0 {
                if nondet() != 0 {
                    x = x +? 1 else panic("overflow")
                } else {
                    y = y -? 1 else panic("overflow")
                    x = nondet()
                }
            } else {
                y = y
            }
        }
    }
}
LN
cat > "$T/over.ln" <<'LN'
func task(x0 i32, M i32) effects raises {
    var x = x0
    if M > 0 {
        while x != M {
            if x > M {
                x = 0
            } else {
                x = x +? 2 else panic("overflow")
            }
        }
    }
}
LN
cat > "$T/larraz.ln" <<'LN'
func task(x0 i32, y0 i32, z0 i32) effects raises {
    var x = x0
    var y = y0
    var z = z0
    if (x <= 10000) and (x >= -10000) and (y <= 10000) and (z <= 10000) {
        while y >= 1 {
            x = x
            while y < z {
                x = x +? 1 else panic("overflow")
                z = z -? 1 else panic("overflow")
            }
            y = x +? y else panic("overflow")
        }
    }
}
LN
cat > "$T/swap.ln" <<'LN'
func task(nd0 i32, nd1 i32, nd2 i32) i32 effects raises {
    var a i32
    var b i32
    var q i32
    var olda i32
    q = nd0
    a = nd1
    b = nd2
    if !((-524287 <= q) and (q <= 524287)) {
        return 0
    }
    if !((-524287 <= a) and (a <= 524287)) {
        return 0
    }
    if !((-524287 <= b) and (b <= 524287)) {
        return 0
    }
    while q > 0 {
        q = (q +? a else panic("overflow")) -? 1 else panic("overflow")
        olda = a
        a = b
        b = olda
    }
    return 0
}
LN
# The teeth are this revision's: K1 (a kept case's representative taken as b = 0) fails negkeep and
# K2 (no keep check) fails shrink. Rerun both whenever the templates in finder.h change: a filter
# that stops a template reaching these loops leaves this test green and vacuous.
cat > "$T/multr.ln" <<'LN'
func mult(n i32, m i32) i32 effects raises {
    if m < 0 {
        return -1 *? mult(n, m) else panic("overflow")
    }
    if m == 0 {
        return 0
    }
    return n +? mult(n, (m -? 1 else panic("overflow"))) else panic("overflow")
}
LN
cat > "$T/twowayr.ln" <<'LN'
func twoWay(terminate i32, n i32) i32 effects raises {
    if n < 0 {
        return 1
    } else {
        var m i32 = n
        if terminate != 0 {
            m = m
        } else {
            m = m +? 1 else panic("overflow")
        }
        return m *? twoWay(terminate, m) else panic("overflow")
    }
}
LN
cat > "$T/ex5r.ln" <<'LN'
func f(x i32, y i32) i32 effects raises {
    if y == 0 {
        return x
    } else {
        if x == 0 {
            return f(y, (y -? 1 else panic("overflow")))
        } else {
            return f(y, x)
        }
    }
}
LN
cat > "$T/swapr.ln" <<'LN'
func f(q i32, a i32, b i32) i32 effects raises {
    if q <= 0 {
        return 0
    }
    return f((q +? a else panic("overflow")) -? 1 else panic("overflow"), b, a)
}
LN
bad=0
for p in negkeep shrink gap over larraz swap multr twowayr ex5r swapr; do
    out=$("$T/jvc" "$T/$p.ln" --judge 2>&1)
    echo "$out" | grep -q '^# judge proved=' || { echo "$p: the driver gave no summary:"; echo "$out" | tail -3; bad=1; continue; }
    recs=$(echo "$out" | awk '$1 == "v1" && $5 ~ /^(rank|rec)\./')
    [ -n "$recs" ] || { echo "$p: no rank.* or rec.* record read (has the record layout moved?)"; bad=1; continue; }
    ni=$(echo "$recs" | awk '$7 != "proved" && $7 != "outside"')
    [ -z "$ni" ] || { echo "$p: a template proposed a ranking the judge does not prove:"; echo "$ni" | cut -c1-160; bad=1; }
    if [ "$p" != shrink ]; then                                 # the ones that run for ever
        echo "$recs" | awk '$7 == "outside"' | grep -q . || { echo "$p: runs for ever, yet no termination record is outside"; bad=1; }
    fi
done
exit $bad
