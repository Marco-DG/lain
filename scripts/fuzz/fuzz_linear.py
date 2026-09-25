#!/usr/bin/env python3
# fuzz_linear.py — generator for the ownership/linearity soundness fuzzer.
# Emits one Lain program to stdout; first line is `// EXPECT: accept|reject`.
#
# The memory-safety core (move / consume / borrow of an owned heap resource) is
# modelled by a REAL malloc'd resource whose consumer frees it and whose "touch"
# derefs it — in two shapes: a bare owned pointer (`mov *u8`) and a struct with a
# linear pointer field (`type Res { mov h *u8 }`). If the linear checker is SOUND,
# every ACCEPTED program consumes each resource exactly once and never touches it
# afterwards — so it must be clean under ASan (no double-free / use-after-free) and
# LSan (no leak) on EVERY control-flow path. A program the checker accepts that is
# nonetheless dirty at runtime is an unsound acceptance.
#
# Two streams (chosen by seed):
#   accept — valid by construction (rich control flow: balanced branches, scoped
#            loops, move-chains, defer, struct resources): must compile AND run clean
#            for BOTH branch directions (the harness flips `flag`).
#   reject — a valid skeleton with exactly one injected linearity violation
#            (leak / double-consume / use-after-consume / move-in-loop / defer-double /
#            unbalanced-branch / conditional-double / struct-double): must NOT compile.
import random, sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
rng = random.Random(seed)

HEADER = """extern func libc_malloc(size usize) mov *void effects io, alloc
extern func libc_free(ptr mov *void) effects io
func acquire() mov *u8 effects io, alloc { unsafe { return libc_malloc(4) as *u8 } }
func release(p mov *u8) effects io { unsafe { libc_free(mov p as *void) } }
func touch(p *u8) u8 { unsafe { return *p } }
type Res { mov h *u8 }
func rmake() Res effects io, alloc { unsafe { return Res(libc_malloc(4) as *u8) } }
func rfree(mov {h} Res) effects io { unsafe { libc_free(mov h as *void) } }
func rtouch(r Res) u8 { unsafe { return *r.h } }
type Two { mov a *u8, mov b *u8 }
func tmake() Two effects io, alloc { unsafe { return Two(libc_malloc(4) as *u8, libc_malloc(4) as *u8) } }
func tfree(mov {a, b} Two) effects io {
    unsafe {
        libc_free(mov a as *void)
        libc_free(mov b as *void)
    }
}"""

class Gen:
    def __init__(self, rng):
        self.rng = rng
        self.lines = []
        self.uid = 0
        self.ind = 1
    def u(self):
        self.uid += 1
        return self.uid
    def emit(self, s):
        self.lines.append("    " * self.ind + s)

    # a bare owned-pointer life: acquire, touch*, consume (direct / balanced-branch / defer).
    def ptr_life(self, name):
        self.emit(f"mov {name} *u8 = acquire()")
        for _ in range(self.rng.randint(0, 2)):
            self.emit(f"var t{self.u()} = touch({name})")
        style = self.rng.random()
        if style < 0.3:
            self.emit(f"if flag {{ release(mov {name}) }} else {{ release(mov {name}) }}")
        elif style < 0.5:
            self.emit(f"defer release(mov {name})")
        else:
            self.emit(f"release(mov {name})")

    # a struct-owned resource life (linear pointer field): make, touch*, consume.
    def struct_life(self, name):
        self.emit(f"var {name} = rmake()")
        for _ in range(self.rng.randint(0, 2)):
            self.emit(f"var t{self.u()} = rtouch({name})")
        if self.rng.random() < 0.3:
            self.emit(f"defer rfree(mov {name})")
        else:
            self.emit(f"rfree(mov {name})")

    # a bounded loop that fully scopes a fresh resource each iteration (always valid).
    def loop_scoped(self):
        i = f"ix{self.u()}"       # avoid iN/uN names (reserved type names)
        s = f"s{self.u()}"
        self.emit(f"var {i} usize = 0")
        self.emit(f"while {i} < 3 decreasing 3 - {i} {{")
        self.ind += 1
        self.emit(f"mov {s} *u8 = acquire()")
        if self.rng.random() < 0.6:
            self.emit(f"var t{self.u()} = touch({s})")
        self.emit(f"release(mov {s})")
        self.emit(f"{i} = {i} + 1")
        self.ind -= 1
        self.emit("}")

    # ── AN ARRAY OF RESOURCES, released element by element ───────────────────────────
    # ★ ADDED 2026-09-23 BECAUSE THIS GENERATOR COULD NOT REACH THE SHAPE. The element rule
    # (D-31) was fail-OPEN for a nested place — `mov a[0].h1` claimed the whole element, so a
    # struct with TWO linear fields inside an array lost one silently. fuzz_linear reported
    # `missed-violations=0` across 250 generations before AND after the fix, because nothing
    # it generates puts a two-obligation value inside an array. A fuzzer's zero is about its
    # generator.
    def array_life(self):
        a = f"ar{self.u()}"
        self.emit(f"var {a} Res[2] = [rmake(), rmake()]")
        self.emit(f"rfree(mov {a}[0])")
        self.emit(f"rfree(mov {a}[1])")

    # a struct with TWO linear fields: both obligations must be discharged, and neither
    # discharge may be mistaken for the other.
    def two_field_life(self):
        t = f"tw{self.u()}"
        self.emit(f"var {t} = tmake()")
        self.emit(f"tfree(mov {t})")

    # move-chain: move an owned ptr into a second var, consume the second.
    def move_chain(self):
        a = f"a{self.u()}"
        b = f"b{self.u()}"
        self.emit(f"mov {a} *u8 = acquire()")
        self.emit(f"mov {b} *u8 = mov {a}")
        self.emit(f"release(mov {b})")

def build_valid(g):
    n = g.rng.randint(1, 4)
    for _ in range(n):
        pick = g.rng.random()
        if pick < 0.28:
            g.ptr_life(f"r{g.u()}")
        elif pick < 0.48:
            g.struct_life(f"q{g.u()}")
        elif pick < 0.62:
            g.loop_scoped()
        elif pick < 0.76:
            g.array_life()
        elif pick < 0.88:
            g.two_field_life()
        else:
            g.move_chain()

def gen_accept():
    g = Gen(rng)
    g.emit("var flag i32 = 1")
    build_valid(g)
    body = "\n".join(g.lines)
    return "// EXPECT: accept\n" + HEADER + "\nfunc main() i32 effects io, raises, alloc {\n" + body + "\n    return 0\n}\n"

def gen_reject():
    kind = rng.choice(["leak", "double", "uaf", "loop_move", "defer_double",
                       "unbalanced", "cond_double", "struct_double",
                       "array_partial", "array_double", "nested_partial"])
    body = ["    var flag i32 = 1"]
    if kind == "leak":
        body.append("    mov r *u8 = acquire()")            # never consumed -> E003
        if rng.random() < 0.5:
            body.append("    var t = touch(r)")
    elif kind == "double":
        body += ["    mov r *u8 = acquire()", "    release(mov r)", "    release(mov r)"]  # E002
    elif kind == "uaf":
        body += ["    mov r *u8 = acquire()", "    release(mov r)", "    var t = touch(r)"]  # E001
    elif kind == "defer_double":
        body += ["    mov r *u8 = acquire()", "    defer release(mov r)", "    release(mov r)"]  # E002
    elif kind == "unbalanced":
        body += ["    mov r *u8 = acquire()", "    if flag { release(mov r) }"]  # E016 (leak on else)
    elif kind == "cond_double":
        body += ["    mov r *u8 = acquire()", "    if flag { release(mov r) }", "    release(mov r)"]  # E016
    elif kind == "struct_double":
        body += ["    var r = rmake()", "    rfree(mov r)", "    rfree(mov r)"]  # E002
    # ── THE THREE SHAPES THIS GENERATOR COULD NOT REACH (added 2026-09-23) ───────────────
    # The element rule (D-31) was fail-OPEN for a nested place and this fuzzer reported
    # `missed-violations=0` right through it, because nothing it generated put a
    # two-obligation value inside an array. A fuzzer's zero is about its generator.
    elif kind == "array_partial":
        body += ["    var ar Res[2] = [rmake(), rmake()]",
                 "    rfree(mov ar[0])"]                     # E003 — ar[1] leaks
    elif kind == "array_double":
        body += ["    var ar Res[2] = [rmake(), rmake()]",
                 "    rfree(mov ar[0])", "    rfree(mov ar[0])",
                 "    rfree(mov ar[1])"]                     # E002 — the same element twice
    elif kind == "nested_partial":
        # ★ THE ONE THAT WAS ACTUALLY MISSED. `mov tw[0].a` names a place INSIDE element 0;
        # resolving it to "element 0" claimed both obligations were discharged and `b` leaked
        # in silence. Two linear fields is the minimum shape that can tell the difference.
        body += ["    var tw Two[1] = [tmake()]",
                 "    unsafe { libc_free(mov tw[0].a as *void) }"]   # E003 — .b leaks
    else:  # loop_move: consume an outer-scope resource inside a loop
        body += ["    mov r *u8 = acquire()", "    var i usize = 0",
                 "    while i < 3 decreasing 3 - i {", "        release(mov r)",
                 "        i = i + 1", "    }"]
    return "// EXPECT: reject\n" + HEADER + "\nfunc main() i32 effects io, raises, alloc {\n" + "\n".join(body) + "\n    return 0\n}\n"

if rng.random() < 0.7:
    sys.stdout.write(gen_accept())
else:
    sys.stdout.write(gen_reject())
