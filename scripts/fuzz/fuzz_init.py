#!/usr/bin/env python3
"""Generate one Lain program that reads storage which may or may not have been written.

"Uninitialised read | UB | Prevented | Prevented" is a row in the README's guarantee table and
it was the last one with NO generative test. The sovereign definite-init pass runs by default
(`--engine=ir`), so the shipping compiler is what gets asked.

The oracle is MemorySanitizer, which tracks uninitialised bits through computation rather than
looking for a sentinel — so a value that is combined, branched on, or narrowed before use is
still caught. A program the compiler ACCEPTS promises no uninitialised read; an MSan report on
one is unsound.

Unsafe variants are generated on purpose. A generator that can only produce initialised
programs proves nothing about the checker.
"""
import random, sys

def gen(rng):
    shape = rng.choice(["whole_init", "no_init", "elem_then_same", "elem_then_other",
                        "comprehension", "loop_fill_all", "loop_fill_half",
                        "branch_one", "branch_both", "struct_partial", "struct_full",
                        "index_store_other", "elem_then_index", "fill_then_index"])
    n = rng.choice([2, 4, 8])
    L = ["extern func libc_printf(fmt *u8, ...) i32 effects io"]

    if shape in ("struct_partial", "struct_full"):
        L += ["type P {", "    x i32", "    y i32", "}"]

    # The index shapes need an index the analysis cannot name: a refined parameter, so the
    # store or read sits in a helper that main calls with a constant. Every other shape is
    # straight-line code in main, where every index is a constant, and a straight-line store
    # at an unknown index was where the hole was: it credited the WHOLE array.
    if shape in ("index_store_other", "elem_then_index", "fill_then_index"):
        L.append(f"func f(j usize < {n}) i32 {{")
        L.append(f"    var a i32[{n}]")
        if shape == "index_store_other":
            L.append("    a[j] = 5")                              # writes a[j] only
            L.append(f"    if a[{n-1}] == 5 {{ return 1 }}")      # main passes j = 0: unwritten
        elif shape == "elem_then_index":
            L.append("    a[0] = 5")
            L.append("    if a[j] == 5 { return 1 }")             # main passes j = n-1: unwritten
        else:  # fill_then_index
            for i in range(n): L.append(f"    a[{i}] = {i+1}")    # every element, so any j is
            L.append("    if a[j] == 5 { return 1 }")             # initialised
        L.append("    return 0")
        L.append("}")
    L.append("func main() i32 effects io, raises, alloc {")
    if shape in ("index_store_other", "elem_then_index", "fill_then_index"):
        j = 0 if shape == "index_store_other" else n - 1
        L.append(f"    libc_printf(\"%d\\n\", f({j}))")
    elif shape == "whole_init":
        L.append(f"    var a i32[{n}] = [{', '.join(str(i+1) for i in range(n))}]")
        L.append("    libc_printf(\"%d\\n\", a[0])")
    elif shape == "no_init":
        L.append(f"    var a i32[{n}]")
        L.append("    libc_printf(\"%d\\n\", a[0])")           # reads storage nothing wrote
    elif shape == "elem_then_same":
        L.append(f"    var a i32[{n}]")
        L.append("    a[0] = 5")
        L.append("    libc_printf(\"%d\\n\", a[0])")           # the element that WAS written
    elif shape == "elem_then_other":
        L.append(f"    var a i32[{n}]")
        L.append("    a[0] = 5")
        L.append(f"    libc_printf(\"%d\\n\", a[{n-1}])")      # a DIFFERENT, unwritten element
    elif shape == "comprehension":
        L.append(f"    var a = [k * 2 for k in 0..{n}]")
        L.append("    libc_printf(\"%d\\n\", a[0])")
    elif shape in ("loop_fill_all", "loop_fill_half"):
        L.append(f"    var a i32[{n}]")
        top = n if shape == "loop_fill_all" else max(1, n // 2)
        L.append(f"    for k in 0..{top} {{ a[k] = 7 }}")
        idx = 0 if shape == "loop_fill_all" else n - 1     # half-fill then read past the fill
        L.append(f"    libc_printf(\"%d\\n\", a[{idx}])")
    elif shape in ("branch_one", "branch_both"):
        L.append("    var flag i32 = 1")
        L.append("    var x i32")
        L.append("    if flag > 0 { x = 1 }")
        if shape == "branch_both":
            L[-1] = "    if flag > 0 { x = 1 } else { x = 2 }"
        L.append("    libc_printf(\"%d\\n\", x)")
    elif shape == "struct_partial":
        L.append("    var p P")
        L.append("    p.x = 3")
        L.append("    libc_printf(\"%d\\n\", p.y)")            # the field nothing wrote
    else:  # struct_full
        L.append("    var p P")
        L.append("    p.x = 3")
        L.append("    p.y = 4")
        L.append("    libc_printf(\"%d\\n\", p.x + p.y)")
    L += ["    return 0", "}"]
    return f"// SHAPE: {shape}\n" + "\n".join(L) + "\n"

if __name__ == "__main__":
    rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else None)
    sys.stdout.write(gen(rng))
