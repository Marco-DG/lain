#!/usr/bin/env python3
"""Generate one Lain program whose `for` ranges may start below zero (I.182).

A range loop's head compare was lowered as UNSIGNED for every counter, and a counter takes its
bounds' type (i32 for literals). The interpreter, the IR's meaning, compared the counter as
uint64, so `for i in -3..2` never ran there, and the analysis agreed with it; the emitted C
compared signed and ran it from -3. fuzz_interp compares the two on every program it is given,
and no generator ever started a range below zero, so the differential never saw it.

Each program runs one to three range loops (signed or unsigned counters; exclusive or inclusive;
bounds from literals or from locals) whose start may be negative. Each loop counts its trips and
sums its counter, and prints both. Some loops index an array at `i - start`, which is in bounds on
every trip of every version; the analysis must prove it, and the run reads it. A program the
compiler accepts must print the same lines natively and under --interpret.
"""
import random, sys

def gen(rng):
    L = ["extern func libc_printf(fmt *u8, ...) i32 effects io",
         "func main() i32 effects io {"]
    for k in range(rng.choice([1, 2, 3])):
        signed = rng.random() < 0.75
        ty = rng.choice(["i32", "i64"]) if signed else rng.choice(["usize", "u32"])
        lo = rng.randint(-6, 4) if signed else rng.randint(0, 4)
        hi = lo + rng.randint(-2, 8)
        if not signed and hi < 0: hi = 0
        incl = rng.random() < 0.3
        dots = "..=" if incl else ".."
        via_locals = rng.random() < 0.5
        if via_locals:
            L.append(f"    var lo{k} {ty} = {lo}")
            L.append(f"    var hi{k} {ty} = {hi}")
            rng_txt = f"lo{k}{dots}hi{k}"
        else:
            rng_txt = f"{lo}{dots}{hi}"
        trips = max(0, (hi - lo + (1 if incl else 0)))
        L.append(f"    var c{k} i64 = 0")
        L.append(f"    var s{k} i64 = 0")
        index = trips > 0 and rng.random() < 0.5 and not via_locals
        if index:
            n = trips
            L.append(f"    var a{k} i64[{n}] = [{', '.join(str(3 * j + 1) for j in range(n))}]")
        L.append(f"    for i in {rng_txt} {{")
        L.append(f"        c{k} = c{k} +% 1")
        L.append(f"        s{k} = s{k} +% (i as i64)")
        if index:
            off = f"(i - {lo})" if signed else (f"(i - {lo})" if lo else "i")
            L.append(f"        s{k} = s{k} +% a{k}[{off}]")
        L.append("    }")
        L.append(f'    libc_printf("{k}: %lld %lld\\n", c{k}, s{k})')
    L.append("    return 0")
    L.append("}")
    return "\n".join(L) + "\n"

if __name__ == "__main__":
    rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else None)
    sys.stdout.write(gen(rng))
