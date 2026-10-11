// src/tools/drv_lower.h — lower a module for an analysis driver AS THE COMPILER DOES IT.
//
// main.c evaluates the module first (DECIDE-W step 2): a module constant whose initialiser calls a
// function is computed by the program's own semantics and lowered as the literal it is. A driver
// that went straight to ir_lower_module analysed an IR the compiler never emits, with that
// initialiser running in `main` and owing obligations the program never has. The judge's driver
// found it as an ASSERT VRA refused in an accepted program (module_const_from_call_pass). Every
// driver lowers through here, with the headers static evaluation needs, in main.c's order.
#ifndef LAIN_TOOLS_DRV_LOWER_H
#define LAIN_TOOLS_DRV_LOWER_H
#include "analysis/vra.h"
#include "ir/emit_c.h"
#include "ir/interp.h"
#include "analysis/linearity.h"
#include "analysis/borrow.h"
#include "analysis/definite_init.h"
#include "analysis/report.h"
#include "frontends/lain/static_eval.h"

static IrFunc *drv_lower_module(DeclList *prog, const char *path, Arena *ia) {
    static Arena ea;
    ea = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE * 4096);
    if (ir_static_eval_module(prog, path, &ea)) { fprintf(stderr, "static evaluation failed\n"); exit(1); }
    return ir_lower_module(prog, ia);
}
#endif
