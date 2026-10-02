#!/usr/bin/env bash
# The IR printer (src/ir/dump.h) names every op and prints every type. It printed nothing for a
# function-pointer type or a vector type, and `??` for IR_CONSUME, so a dump of a program with a
# function pointer, a `Vec` or a `mov` was missing exactly those facts (the Debugger agent's
# reading). This walks every IrOp and every IrTypeKind, so an op or a kind added later without a
# case here fails too, and pins the spellings: Lain's own `*func(..) R` and `Vec(N, T)`.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/p.c" <<'C'
#include <stdio.h>
#include <string.h>
#include "ir/ir.h"
#include "ir/dump.h"
static const char *show(IrType *t) {
    static char buf[256]; FILE *m = tmpfile(); ir_dump_type(t, m);
    long n = ftell(m); rewind(m); n = (long)fread(buf, 1, n < 255 ? n : 255, m); buf[n] = 0; fclose(m);
    return buf;
}
int main(void) {
    int bad = 0;
    for (int op = 0; op <= IR_PHI; op++)
        if (!strcmp(ir_op_name((IrOp)op), "??")) { printf("op %d has no name\n", op); bad++; }
    for (int k = 0; k <= IRT_NEVER; k++) {
        IrType t; memset(&t, 0, sizeof t); t.kind = (IrTypeKind)k;
        if (!show(&t)[0]) { printf("type kind %d prints nothing\n", k); bad++; }
    }
    IrType i32, u8, i64, fn, fn0, vec;
    memset(&i32, 0, sizeof i32); i32.kind = IRT_INT; i32.bits = 32; i32.is_signed = 1;
    u8 = i32; u8.bits = 8; u8.is_signed = 0;  i64 = i32; i64.bits = 64;
    IrType *ps[2] = { &i32, &u8 };
    memset(&fn, 0, sizeof fn);  fn.kind = IRT_FUNC; fn.fields = ps; fn.n_fields = 2; fn.elem = &i64;
    memset(&fn0, 0, sizeof fn0); fn0.kind = IRT_FUNC;
    memset(&vec, 0, sizeof vec); vec.kind = IRT_VECTOR; vec.elem = &i32; vec.array_len = 4;
    struct { IrType *t; const char *want; } cs[] = {
        { &fn, "*func(i32, u8) i64" }, { &fn0, "*func()" }, { &vec, "Vec(4, i32)" } };
    for (int i = 0; i < 3; i++)
        if (strcmp(show(cs[i].t), cs[i].want)) { printf("printed `%s`, want `%s`\n", show(cs[i].t), cs[i].want); bad++; }
    if (strcmp(ir_op_name(IR_CONSUME), "consume")) { printf("IR_CONSUME is `%s`\n", ir_op_name(IR_CONSUME)); bad++; }
    if (!bad) printf("every op named, every type kind printed\n");
    return bad != 0;
}
C
gcc -std=c99 -w -I "$ROOT/src" -o "$D/p" "$D/p.c" || { echo "the probe does not build"; exit 1; }
"$D/p"
