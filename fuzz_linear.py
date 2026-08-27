#!/usr/bin/env python3
# fuzz_linear.py — generator for the ownership/linearity soundness fuzzer.
# Emits one Lain program to stdout; first line is `// EXPECT: accept|reject`.
#
# The memory-safety core (move / consume / borrow of an owned heap pointer) is
# modelled by a REAL malloc'd resource whose consumer frees it and whose "touch"
# derefs it. If the linear checker is SOUND, every ACCEPTED program consumes each
# resource exactly once and never touches it afterwards — so it must be clean
# under ASan (no double-free / use-after-free) and LSan (no leak). A program the
# checker accepts that is nonetheless dirty at runtime is an unsound acceptance.
#
# Two streams (chosen by seed):
#   accept  — valid by construction (rich control flow): must compile AND run clean.
#   reject  — a valid skeleton with exactly one injected linearity violation
#             (leak / double-consume / use-after-consume / move-in-loop): must NOT
#             compile. If it compiles it is run anyway — dirty => unsound.
import random, sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
rng = random.Random(seed)

HEADER = """extern proc libc_malloc(size usize) mov *void
extern proc libc_free(ptr mov *void)
proc acquire() mov *u8 { unsafe { return libc_malloc(4) as *u8 } }
proc release(p mov *u8) { unsafe { libc_free(mov p as *void) } }
proc touch(p *u8) u8 { unsafe { return *p } }"""

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

    # a self-contained resource life inside the current scope: acquire, touch*, consume.
    # `consume_style`: direct | branch (balanced, both arms free).
    def resource_life(self, name):
        self.emit(f"mov {name} *u8 = acquire()")
        for _ in range(self.rng.randint(0, 2)):
            self.emit(f"var t{self.u()} = touch({name})")
        if self.rng.random() < 0.4:
            # balanced branch: both arms consume -> consumed after merge
            self.emit(f"if flag {{ release(mov {name}) }} else {{ release(mov {name}) }}")
        else:
            self.emit(f"release(mov {name})")

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
        if pick < 0.5:
            g.resource_life(f"r{g.u()}")
        elif pick < 0.75:
            g.loop_scoped()
        else:
            g.move_chain()

def gen_accept():
    g = Gen(rng)
    g.emit("var flag i32 = 1")
    build_valid(g)
    body = "\n".join(g.lines)
    return "// EXPECT: accept\n" + HEADER + "\nproc main() i32 {\n" + body + "\n    return 0\n}\n"

def gen_reject():
    # a single resource with exactly one injected violation.
    kind = rng.choice(["leak", "double", "uaf", "loop_move"])
    body = ["    var flag i32 = 1"]
    if kind == "leak":
        body.append("    mov r *u8 = acquire()")            # never consumed -> E003
        if rng.random() < 0.5:
            body.append("    var t = touch(r)")
    elif kind == "double":
        body.append("    mov r *u8 = acquire()")
        body.append("    release(mov r)")
        body.append("    release(mov r)")                    # double-consume -> E002
    elif kind == "uaf":
        body.append("    mov r *u8 = acquire()")
        body.append("    release(mov r)")
        body.append("    var t = touch(r)")                  # use-after-consume -> E001
    else:  # loop_move: consume an outer-scope resource inside a loop
        body.append("    mov r *u8 = acquire()")
        body.append("    var i usize = 0")
        body.append("    while i < 3 decreasing 3 - i {")
        body.append("        release(mov r)")                # re-consumed each iter
        body.append("        i = i + 1")
        body.append("    }")
    return "// EXPECT: reject\n" + HEADER + "\nproc main() i32 {\n" + "\n".join(body) + "\n    return 0\n}\n"

if rng.random() < 0.7:
    sys.stdout.write(gen_accept())
else:
    sys.stdout.write(gen_reject())
