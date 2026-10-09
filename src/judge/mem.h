#ifndef LAIN_JUDGE_MEM_H
#define LAIN_JUDGE_MEM_H
// mem.h — the judge's allocator: a bump region over memory its CALLER provides. Running out is a
// result (NULL), never an abort. The judge is a library with no exit() and no static state
// (review G5), and the compiler's arena (utils/arena.h) panics when full, so the judge does
// not use it.
#include <stddef.h>
#include <stdint.h>

typedef struct { char *beg, *cur, *end; } JMem;

static JMem jmem_over(void *buf, size_t size) {
    JMem m; m.beg = m.cur = (char *)buf; m.end = (char *)buf + size; return m;
}
// `n` objects of `size` bytes, aligned to 16 (the widest member the judge's types hold is 8).
// NULL if they do not fit, or if n is negative; a zero count yields a valid, empty pointer.
static void *jmem_alloc(JMem *m, size_t size, int64_t n) {
    if (n < 0) return NULL;
    uintptr_t at = ((uintptr_t)m->cur + 15u) & ~(uintptr_t)15u;
    size_t room = (size_t)(m->end - m->beg);
    size_t used = (size_t)(at - (uintptr_t)m->beg);
    if (used > room) return NULL;
    if (size && (uint64_t)n > (room - used) / size) return NULL;
    m->cur = (char *)at + size * (size_t)n;
    return (void *)at;
}
#define JMEM_NEW(m, T, n) ((T *)jmem_alloc((m), sizeof(T), (int64_t)(n)))

#endif // LAIN_JUDGE_MEM_H
