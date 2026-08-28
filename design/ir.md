# Lain IR — Specification

*Phase 0.2 of the clean-core rebuild (see `REBUILD.md`, `design/architecture.md`). The
typed SSA/CFG intermediate representation that the new analyses run over and the backend
lowers to C. Designed **provability-first**: value identity, explicit control flow, and a
shape the abstract interpreter can consume directly.*

---

## 1. Design goals

1. **Value identity, not names.** Every value is defined exactly once (SSA) and identified
   by its definition, killing the name-keying that pervades `src/sema/`.
2. **Explicit control flow.** A function is a control-flow graph of basic blocks. Loops are
   back-edges; joins are φ-nodes. Dataflow analysis is a fixpoint over this graph — the VRA
   becomes a textbook abstract interpreter, not an AST walk.
3. **Typed and total.** Every value carries an IR type; every block ends in a terminator;
   the CFG is complete. No pass ever re-parses or re-resolves.
4. **Analysis-facing structure is first-class.** A slice's length is a *value*
   (`slice.len`), an array index is an explicit `index` the bounds consumer checks, a call
   carries its callee. The information the proof engine needs is in the IR, not recovered by
   pattern-matching syntax.
5. **Soundness firewall.** Arithmetic is over mathematical integers ℤ; overflow is a
   *separate* proof obligation on the result, not baked into the instruction. The backend is
   dumb; optimization is a later IR→IR pass that cannot affect the checks.

## 2. Types (`IrType`)

Mirror Lain's type system to the extent analysis + codegen need:

| IR type            | meaning                                              | analysis-relevant facts |
|--------------------|------------------------------------------------------|-------------------------|
| `Int{bits,signed}` | iN / uN (N∈1..64), usize/isize as i64/u64            | width + signedness ⇒ the type interval [lo,hi]; overflow target |
| `Bool`             | i1 truth value                                       | {0,1}                   |
| `Float{32\|64}`    | f32 / f64                                            | opaque to the numeric domain (for now) |
| `Ptr{pointee,mut}` | raw `*T` / `*var T`                                  | mutability for borrow; deref is `unsafe` |
| `Slice{elem,sent}` | `T[]` / `u8[:0]` — a fat value {data ptr, len}       | carries a `len` value ⇒ bounds |
| `Array{elem,n}`    | fixed `T[N]`                                         | constant length N       |
| `Struct{decl}`     | named aggregate                                      | field layout            |
| `Unit`             | no value (`proc` with no return)                     | —                       |
| `Never`            | diverges (`panic`, `unreachable`)                    | bottom; unreachable code |

`irtype_int_range(t) → [lo,hi]` gives the type interval used to seed the numeric domain and
to check narrowing/overflow.

## 3. Values and the memory model

- **`IrValue`** = a typed SSA name with a defining instruction (or a block parameter /
  function parameter). Constants are `IrValue`s produced by `const`.
- **SSA for temporaries.** Every expression result is a fresh single-assignment value.
- **Mutable locals via memory, then promoted.** A Lain `var x` becomes an `alloca` (a stack
  slot); reads are `load`, writes are `store`. This makes lowering trivial and correct with
  **no SSA construction up front**. A **`mem2reg`** pass then promotes non-address-taken
  slots to SSA values + φ-nodes. *Staging:* Phase 1 may run on the memory form directly (the
  octagon domain simply tracks the stack slots as its variables); `mem2reg` is a precision/
  cleanliness upgrade, not a correctness prerequisite.
- **Block parameters vs. φ.** We use classic **φ-nodes** at block heads (equivalent to block
  parameters). The join of two branches introduces `φ [v_then: B_then, v_else: B_else]`.

## 4. Instructions (`IrInstr`)

Pure/effectful is a property, not a syntactic category. Each instruction defines 0 or 1
value.

**Constants & moves**
- `const K : T` — integer/float/bool/unit literal.

**Arithmetic (over ℤ; result range is a domain fact, overflow a separate check)**
- `add, sub, mul, sdiv, udiv, srem, urem, neg`
- `and, or, xor, shl, lshr, ashr, bnot`
- Wrapping/saturating variants carry a flag (`add.wrap`, `add.sat`) — the checker skips the
  overflow obligation for `.wrap`, clamps the range for `.sat`.

**Comparison → `Bool`**
- `icmp {eq,ne,slt,sle,sgt,sge,ult,ule,ugt,uge} a, b`

**Aggregates, slices, memory**
- `alloca T` → `Ptr{T}` (a mutable local slot)
- `load p` , `store p, v`
- `field_ptr base, k` — address of struct field k
- `elem_ptr base, idx` — address of array/slice element **idx** (the bounds consumer proves
  `0 ≤ idx < len(base)` here; `unsafe` marks it unchecked)
- `slice_len s` → the length value of a slice (first-class ⇒ the domain relates it to indices)
- `slice_data s` , `make_slice data, len` , `subslice s, lo, hi`
- `array_new [v0,…]` , `struct_new decl, [f0,…]`

**Calls & casts**
- `call f, [args]` → result; carries the callee decl (⇒ return refinements, effects, purity)
- `cast.{zext,sext,trunc,itof,ftoi,bitcast,ptr} v : T` — `trunc`/narrowing carries the check
  obligation

**SSA**
- `phi [(v_i, B_i)…] : T`

## 5. Terminators (end every block)

- `br B` — unconditional
- `br_cond c, B_then, B_else` — the **guard**: the VRA refines the domain by `c` on the true
  edge and by `¬c` on the false edge (this is where conditional refinement lives, cleanly)
- `switch v, [(k_i → B_i)…], default → B_d` — `match` on an integer/tag
- `ret v?`
- `unreachable` — after `panic`/diverging call (type `Never`)

## 6. Blocks, CFG, functions, module

- **`IrBlock`** = label + φ-nodes + instructions + one terminator. Knows its predecessors
  and successors. A block that is a branch/switch target of a back-edge is a **loop header**
  (marked during lowering / by a dominator+back-edge pass) — the widening point.
- **`IrFunc`** = name + typed signature (params are `IrValue`s) + entry block + block list +
  `kind` (func/proc) + effect row + optional return refinement.
- **`IrModule`** = functions + type/struct declarations + externs.

Analyses receive the `IrFunc` and a fixpoint engine (`analysis/fixpoint.h`) that iterates the
CFG to convergence; they supply only the abstract domain and the per-instruction transfer
function.

## 7. AST → IR lowering (`src/ir/lower.h`)

Reuses name-resolution + type-checking, then:

- **Expression** → recursively lower operands to values, emit the instruction, yield its
  result value. `a.len` (slice) → `slice_len`. `a[i]` → `elem_ptr a, i` then `load`.
- **`x = e` (var)** → `store slot_x, lower(e)`. **`e` (read of x)** → `load slot_x`.
- **`if c { T } else { E }`** → `br_cond` to `B_T` / `B_E`; both `br B_join`; a value live-out
  of both branches becomes `φ` in `B_join`. *(Note: the domain **join** at `B_join` is what
  makes the current "restore-not-join" bug structurally impossible.)*
- **`while c { body }`** → `B_head:` evaluate `c`, `br_cond` to `B_body` / `B_exit`;
  `B_body` … `br B_head` (back-edge). `B_head` is the loop header (widening point). A
  `decreasing` measure / inferred termination becomes a fact proven on this CFG.
- **`for v in a..b`** → desugar to the `while` shape with an induction counter.
- **`match`** → `switch` on the discriminant; arms are blocks joining at `B_join`.
- **`break` / `continue`** → `br B_exit` / `br B_head` — control flow, not a special case.
- **`defer` / linear consumption** → scheduled stores/calls at scope-exit edges (ownership
  pass consumes this).

Because control flow is explicit, the analyses no longer special-case early-return,
post-loop state, or break — those are ordinary CFG edges.

## 8. IR → C backend (`src/ir/emit_c.h`)

Faithful and dumb:
- Each `IrFunc` → a C function; each `IrBlock` → a label; terminators → `goto` /
  `if(c) goto` / `switch` / `return`.
- φ-nodes are eliminated the standard way: on each incoming edge, assign the φ's value into a
  slot read at the head.
- Instructions → the obvious C. `elem_ptr`/`load`/`store` → array/pointer C; checks were
  already discharged by analysis, so **no runtime checks are emitted** (the check-free
  promise). `.wrap`/`.sat` → the existing modular/clamping helpers.
- Correctness over prettiness (gotos are fine); structured-CFG reconstruction is a later
  nicety.

## 9. Why this shape is provability-first (the payoff)

| current pain (`src/sema/`)                  | how the IR removes it |
|---------------------------------------------|-----------------------|
| name-keyed facts, string compares everywhere| SSA value identity |
| if-merge *restores* pre-state (no join)     | φ-nodes ⇒ domain **join** at every merge, by construction |
| loops: blanket-widen then recover by recognizer | CFG back-edge + **fixpoint + widening** ⇒ invariants *discovered* |
| ~70 syntactic recognizers                   | general transfer functions over a relational domain; most idioms fall out |
| relational facts inexpressible (`i≤j`, `o=i+j`) | octagon domain expresses `±x ± y ≤ c` natively |
| early-return / break / post-loop special-cased | ordinary CFG edges |

## 10. Worked example (illustrative)

Lain:
```
func sum(a i32[]) i32 {
    var s = 0
    var i usize = 0
    while i < a.len { s = s + a[i]  i = i + 1 }
    return s
}
```
IR (sketch, memory form before mem2reg):
```
func sum(a: Slice{i32}) -> i32:
entry:
    s   = alloca i32 ; store s, const 0
    i   = alloca u64 ; store i, const 0
    br head
head:                                   ; loop header (widening point)
    iv  = load i
    n   = slice_len a
    c   = icmp ult iv, n
    br_cond c, body, exit
body:
    iv1 = load i
    p   = elem_ptr a, iv1               ; bounds consumer proves 0 ≤ iv1 < n here
    x   = load p
    sv  = load s ; sv1 = add sv, x ; store s, sv1
    iv2 = add iv1, const 1 ; store i, iv2
    br head
exit:
    r   = load s ; ret r
```
On this CFG the VRA proves `iv < n` at `elem_ptr` as the ordinary loop invariant produced by
the fixpoint (the guard `c` refines the true edge into `body`), with **no** "canonical scan"
recognizer. That is the whole point.

## 11. Open items (finalize with the VRA design, 0.3)
- Exact φ vs. block-parameter form (equivalent; pick one and be consistent).
- Whether `mem2reg` runs in Phase 1 or is deferred to Phase 2 (leaning: defer; run the domain
  on stack slots first).
- How overflow/narrowing obligations are attached (per-instruction flag vs. a side table) —
  leaning: a flag on the arithmetic/`trunc`/`cast` instruction, discharged by the VRA consumer.
- Struct/slice value vs. pointer conventions at the C ABI boundary (reuse current emit rules).
