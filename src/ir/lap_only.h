// lap_only.h — PE.12: WHAT A LOOP COMPUTES THAT NOTHING ELSE IN IT READS.
//
// A loop that counts its laps (`n = n +% 1`, n read by no test) never comes back to a state it was
// in, though everything it decides on does: of fuzz_termination's 79 hanging loops, a witness's run
// confirmed 0 with the counter and 74 with the line removed (TCW, 2026-10-25). This is the part of
// a header's state a repeat may leave out.
//
// THE REGION is the blocks reachable from the header that reach it again. A cell is LAP-ONLY when
// it is a scalar integer or bool local (an alloca of one) whose address is used nowhere in the
// function but as a load's or a store's address, and every load of it in the region is lap-only.
// A value is LAP-ONLY when an instruction in the region computes it by an operation that cannot
// stop the run (wrapping + - *, & | ^ ~, a comparison of two integers, a load of a lap-only cell),
// and every use of it in the region is a lap-only operation or a store into a lap-only cell. A
// checked `n = n + 1` is not: its overflow ends the run, so n stays state.
//
// WHY A REPEAT OF THE REST IS A HANG. Every block run between two visits of the header is in the
// region: it was reached from the header, and the header is reached from it. There, no test, call,
// return, check or store into another cell receives a lap-only value, and a lap-only cell's address
// reaches no callee and no other value. So each step from a visit does the same thing to the rest
// of the state whatever the lap-only part holds: the same instruction, the same value for each
// value that is not lap-only, the same branch, the same trap or none; a lap-only operation itself
// never traps. A lap-only cell read in the region was written before (a load of it unwritten would
// have stopped the first run), and stores only add to what is written. If the rest of the state at
// a later visit equals an earlier visit's, the run from the later one repeats the laps between
// them, and then again, forever.
//
// Uses OUTSIDE the region (`return n` after the loop) are not reads: a run that repeats never
// reaches them.
#ifndef LAIN_IR_LAP_ONLY_H
#define LAIN_IR_LAP_ONLY_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"

enum { IR_LAP_STATE = 0, IR_LAP_VALUE = 1, IR_LAP_CELL = 2 };

// a refined integer is not: a value outside its refinement stops the run where it lands
static bool ir_lap_scalar(const IrType *t) {
    return t && (t->kind == IRT_BOOL || (t->kind == IRT_INT && !t->has_refine && !t->has_ne));
}
static bool ir_lap_same(const IrType *a, const IrType *b) {
    return a && b && a->kind == b->kind && (a->kind != IRT_INT || (a->bits == b->bits && a->is_signed == b->is_signed));
}
// an operation that cannot stop the run, on scalar integers or bools (the one-instruction test;
// a load also needs its cell to be a candidate)
static bool ir_lap_op(const IrInstr *i) {
    if (!i->result || !ir_lap_scalar(i->result->type)) return false;
    switch (i->op) {
        case IR_ADD: case IR_SUB: case IR_MUL: return i->wrap == IR_WRAP_MODULAR;
        case IR_AND: case IR_OR: case IR_XOR: case IR_BNOT: return true;
        case IR_ICMP:
            for (int k = 0; k < i->n_operands; k++) if (!i->operands[k] || !ir_lap_scalar(i->operands[k]->type)) return false;
            return true;
        default: return false;
    }
}
static void ir_lap_unmark(uint8_t *lap, int32_t n, const IrValue *v, bool *changed) {
    if (v && v->id >= 0 && v->id < n && lap[v->id]) { lap[v->id] = 0; *changed = true; }
}
// out[id] for each value of f (out has f->next_value_id entries): IR_LAP_CELL a lap-only cell the
// region stores into, IR_LAP_VALUE any other lap-only value or cell, IR_LAP_STATE the rest. False
// (out all IR_LAP_STATE) when there is no memory to decide it.
static bool ir_lap_only(IrFunc *f, const IrBlock *H, uint8_t *out) {
    int32_t nv = f->next_value_id > 0 ? f->next_value_id : 1, nb = f->next_block_id > 0 ? f->next_block_id : 1;
    memset(out, IR_LAP_STATE, (size_t)nv);
    if (!H) return false;
    uint8_t *fw = calloc((size_t)nb, 1), *bw = calloc((size_t)nb, 1), *cell = calloc((size_t)nv, 1);
    const IrBlock **st = malloc(sizeof *st * (size_t)nb);
    bool ok = fw && bw && cell && st;
    for (const IrBlock *b = f->blocks; ok && b; b = b->next) ok = b->id >= 0 && b->id < nb;
    if (!ok) { free(fw); free(bw); free(cell); free(st); return false; }
    // the region: forward from H over the terminators, backward to H over the predecessors
    int32_t sp = 0;
    fw[H->id] = 1; st[sp++] = H;
    while (sp) {
        const IrBlock *b = st[--sp]; const IrBlock *s[2] = { b->term.a, b->term.b };
        for (int k = 0; k < 2; k++) if (s[k] && !fw[s[k]->id]) { fw[s[k]->id] = 1; st[sp++] = s[k]; }
        for (const IrSwitchCase *c = b->term.cases; c; c = c->next)
            if (c->target && !fw[c->target->id]) { fw[c->target->id] = 1; st[sp++] = c->target; }
    }
    bw[H->id] = 1; st[sp++] = H;
    while (sp) {
        const IrBlock *b = st[--sp];
        for (const IrEdge *e = b->preds; e; e = e->next)
            if (e->block && !bw[e->block->id]) { bw[e->block->id] = 1; st[sp++] = e->block; }
    }
    // candidate cells: a scalar local whose address is only ever a load's or a store's address
    for (const IrBlock *b = f->blocks; b; b = b->next)
        for (const IrInstr *i = b->instrs; i; i = i->next)
            if (i->op == IR_ALLOCA && i->result && i->n_operands == 0 && !i->data && !i->seal && ir_lap_scalar(i->aux.alloca_ty) &&
                i->result->id >= 0 && i->result->id < nv)
                cell[i->result->id] = 1;
    for (const IrBlock *b = f->blocks; b; b = b->next) {
        for (const IrInstr *i = b->phis; i; i = i->next)
            for (const IrPhiArg *a = i->phi_args; a; a = a->next)
                if (a->value && a->value->id >= 0 && a->value->id < nv) cell[a->value->id] = 0;
        for (const IrInstr *i = b->instrs; i; i = i->next)
            for (int k = 0; k < i->n_operands; k++) {
                const IrValue *v = i->operands[k];
                if (!v || v->id < 0 || v->id >= nv) continue;
                if (k == 0 && (i->op == IR_LOAD || i->op == IR_STORE)) continue;
                cell[v->id] = 0;
            }
        if (b->term.cond && b->term.cond->id >= 0 && b->term.cond->id < nv) cell[b->term.cond->id] = 0;
    }
    // start from every candidate, and unmark until nothing in the region reads a marked value
    // but a marked operation or a store into a marked cell
    uint8_t *lap = out;
    for (int32_t v = 0; v < nv; v++) lap[v] = cell[v];
    for (const IrBlock *b = f->blocks; b; b = b->next) {
        if (!fw[b->id] || !bw[b->id]) continue;
        for (const IrInstr *i = b->instrs; i; i = i->next) {
            if (!i->result || i->result->id < 0 || i->result->id >= nv) continue;
            bool l = i->op == IR_LOAD ? i->n_operands >= 1 && i->operands[0] && i->operands[0]->id >= 0 &&
                                        i->operands[0]->id < nv && cell[i->operands[0]->id] && ir_lap_scalar(i->result->type)
                                      : ir_lap_op(i);
            if (l) lap[i->result->id] = 1;
        }
    }
    for (bool changed = true; changed; ) {
        changed = false;
        for (const IrBlock *b = f->blocks; b; b = b->next) {
            if (!fw[b->id] || !bw[b->id]) continue;
            for (const IrInstr *i = b->phis; i; i = i->next)
                for (const IrPhiArg *a = i->phi_args; a; a = a->next) ir_lap_unmark(lap, nv, a->value, &changed);
            for (const IrInstr *i = b->instrs; i; i = i->next) {
                bool marked = i->result && i->result->id >= 0 && i->result->id < nv && lap[i->result->id];
                if (i->op == IR_STORE && i->n_operands == 2) {
                    const IrValue *a = i->operands[0], *x = i->operands[1];
                    bool into = a && a->id >= 0 && a->id < nv && cell[a->id] && lap[a->id];
                    // a lap-only value lands in its own type, which holds it (a narrowing could trap)
                    if (into && x && x->id >= 0 && x->id < nv && lap[x->id] && !ir_lap_same(x->type, a->type && a->type->kind == IRT_PTR ? a->type->elem : NULL))
                        ir_lap_unmark(lap, nv, a, &changed), into = false;
                    if (!into) ir_lap_unmark(lap, nv, x, &changed);
                    continue;
                }
                if (i->op == IR_LOAD && i->n_operands >= 1 && i->operands[0] && i->operands[0]->id >= 0 &&
                    i->operands[0]->id < nv && cell[i->operands[0]->id]) {
                    if (!marked) ir_lap_unmark(lap, nv, i->operands[0], &changed);   // read into the state
                    continue;
                }
                if (marked) continue;                     // a lap-only operation may read anything
                for (int k = 0; k < i->n_operands; k++) ir_lap_unmark(lap, nv, i->operands[k], &changed);
            }
            ir_lap_unmark(lap, nv, b->term.cond, &changed);
        }
    }
    // a cell the region stores into is named as such (the note says it changes)
    for (const IrBlock *b = f->blocks; b; b = b->next) {
        if (!fw[b->id] || !bw[b->id]) continue;
        for (const IrInstr *i = b->instrs; i; i = i->next)
            if (i->op == IR_STORE && i->n_operands == 2 && i->operands[0] && i->operands[0]->id >= 0 &&
                i->operands[0]->id < nv && cell[i->operands[0]->id] && lap[i->operands[0]->id])
                lap[i->operands[0]->id] = IR_LAP_CELL;
    }
    free(fw); free(bw); free(cell); free(st);
    return true;
}

#endif
