// src/ir/test_ir.c — golden test for the IR construction API + dumper (Phase 1.2).
//
// Builds two functions' IR by hand and prints them, exercising values, blocks, the
// alloca/load/store memory model, branches, a loop (with automatic loop-header
// detection via the back-edge), and the dumper. Standalone — NOT part of the main
// build (`gcc src/main.c`), which is untouched.
//
// Build & run:
//   gcc -std=c99 -o /tmp/test_ir src/ir/test_ir.c -I . && /tmp/test_ir
//
// Expected shape (ids are dense per function):
//   func maxi(%0,%1) -> i32  : icmp.sgt → br_cond → two ret blocks
//   func count(%0)   -> i32  : alloca/store; while as head/body/exit; bb1 = loop header
#include "build.h"
#include "dump.h"

int main(void) {
    Arena a = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE * 256);
    IrType *i32 = ir_type_int(&a, 32, true);

    // func maxi(a i32, b i32) i32 { if a > b { return a } return b }
    {
        IrFunc *f = ir_func_new(&a, id(&a, 4, "maxi"), i32, IR_FUNC_PURE);
        IrValue *pa = ir_add_param(f, i32, id(&a, 1, "a"));
        IrValue *pb = ir_add_param(f, i32, id(&a, 1, "b"));
        IrValue *c  = ir_icmp(f, f->entry, IR_CMP_SGT, pa, pb);
        IrBlock *tb = ir_new_block(f), *eb = ir_new_block(f);
        ir_set_br_cond(f->entry, c, tb, eb);
        ir_set_ret(tb, pa);
        ir_set_ret(eb, pb);
        ir_finalize_cfg(f);
        ir_dump_func(f, stdout);
        fputc('\n', stdout);
    }

    // func count(n i32) i32 { var i = 0; while i < n { i = i + 1 } return i }
    {
        IrFunc *f = ir_func_new(&a, id(&a, 5, "count"), i32, IR_FUNC_PURE);
        IrValue *pn = ir_add_param(f, i32, id(&a, 1, "n"));
        IrBlock *e  = f->entry;
        IrValue *islot = ir_alloca(f, e, i32);
        ir_store(f, e, islot, ir_const_int(f, e, 0, i32));
        IrBlock *head = ir_new_block(f), *body = ir_new_block(f), *exit = ir_new_block(f);
        ir_set_br(e, head);
        IrValue *iv = ir_load(f, head, islot, i32);
        ir_set_br_cond(head, ir_icmp(f, head, IR_CMP_SLT, iv, pn), body, exit);
        IrValue *iv1 = ir_load(f, body, islot, i32);
        ir_store(f, body, islot, ir_binop(f, body, IR_ADD, iv1, ir_const_int(f, body, 1, i32), i32));
        ir_set_br(body, head);
        ir_set_ret(exit, ir_load(f, exit, islot, i32));
        ir_finalize_cfg(f);
        ir_dump_func(f, stdout);
    }
    return 0;
}
