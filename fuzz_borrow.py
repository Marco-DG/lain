#!/usr/bin/env python3
# fuzz_borrow.py — generator for the borrow/region (dangling-reference) fuzzer.
# Emits one Lain program to stdout; first line is `// EXPECT: accept|reject`.
#
# A returned SLICE is the ASan-observable borrow surface: indexing a slice is SAFE
# syntax (no `unsafe`), so a dangling slice the checker wrongly accepted derefs freed
# stack memory at runtime — caught under ASan with detect_stack_use_after_return=1.
# If the region checker is SOUND, every ACCEPTED program's returned slice still points
# at live memory when indexed, so it is clean.
#
# Two streams:
#   accept — the returned slice borrows from a PARAMETER (the caller's array, which
#            outlives the call) or the caller's own local: indexing is in-bounds and
#            live. Must compile AND run clean.
#   reject — the returned slice borrows a callee LOCAL array (dangles once the frame
#            pops): must NOT compile (E010). If it compiles it is indexed anyway;
#            a stack-use-after-return trap => unsound.
import random, sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
rng = random.Random(seed)

def gen_accept():
    n = rng.randint(2, 6)
    lo = rng.randint(0, n - 1)
    hi = rng.randint(lo + 1, n)
    idx = rng.randint(0, hi - lo - 1)
    vals = ", ".join(str(rng.randint(0, 99)) for _ in range(n))
    style = rng.choice(["param_whole", "param_sub", "local_direct"])
    lines = ["// EXPECT: accept"]
    # canonical check-free scan of the returned slice: `while i < s.len { sum += s[i] }`
    # (VRA proves it in-bounds). It DEREFS every element, so a dangling slice the
    # checker wrongly accepted traps under ASan (stack-use-after-return).
    scan = [
        "    var sum i32 = 0",
        "    var i usize = 0",
        "    while i < s.len decreasing s.len - i {",
        "        sum = sum + s[i]",
        "        i = i + 1",
        "    }",
        "    return sum - sum",
    ]
    if style == "param_whole":
        lines += [
            "func borrow(a i32[]) i32[] {",
            "    return a",
            "}",
            "proc main() i32 {",
            f"    var arr i32[{n}] = [{vals}]",
            "    var s = borrow(arr)",
        ] + scan + ["}"]
    elif style == "param_sub":
        lines += [
            "func borrow(a i32[]) i32[] {",
            f"    return a[{lo}..{hi}]",
            "}",
            "proc main() i32 {",
            f"    var arr i32[{n}] = [{vals}]",
            "    var s = borrow(arr)",
        ] + scan + ["}"]
    else:  # local_direct: slice a local IN THE SAME frame that scans it (no escape).
        lines += [
            "proc main() i32 {",
            f"    var arr i32[{n}] = [{vals}]",
            f"    var s = arr[{lo}..{hi}]",
        ] + scan + ["}"]
    return "\n".join(lines) + "\n"

def gen_reject():
    n = rng.randint(2, 6)
    vals = ", ".join(str(rng.randint(0, 99)) for _ in range(n))
    kind = rng.choice(["ret_local_slice", "ret_local_ref"])
    lines = ["// EXPECT: reject"]
    if kind == "ret_local_slice":
        lines += [
            "func dangling() i32[] {",
            f"    var a i32[{n}] = [{vals}]",
            "    return a",                       # slice of local -> E010
            "}",
            "proc main() i32 {",
            "    var s = dangling()",
            "    return s[0]",
            "}",
        ]
    else:  # ret_local_ref: return a mutable reference to a local
        lines += [
            "func dangling() var i32 {",
            "    var x = 7",
            "    return var x",                    # &local -> E010
            "}",
            "proc main() i32 {",
            "    return 0",
            "}",
        ]
    return "\n".join(lines) + "\n"

if rng.random() < 0.6:
    sys.stdout.write(gen_accept())
else:
    sys.stdout.write(gen_reject())
