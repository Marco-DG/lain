#ifndef UTILS_ARENA_H
#define UTILS_ARENA_H

/*
    from: https://nullprogram.com/blog/2023/09/27/
*/

#include "common/def.h"     /* usize, isize, uptr */
#include "common/predef.h"  /* nodiscard, inline, alignof */
#include "panic.h"          /* panic_if_msg */

#ifndef ARENA_DEBUG
#define ARENA_DEBUG 1
#endif

// Every push is aligned to its type (I.170): the unaligned form placed Expr, Stmt and ExprList
// nodes at odd addresses behind a string, a misaligned access that is undefined in C and that a
// UBSan-built compiler reported on 30 corpus programs. Pushes of one type stay contiguous, since
// a type's size is a multiple of its alignment.
#define arena_push(arena, type)                     (type *)_arena_push_aligned(arena, sizeof(type), 1, alignof(type))
#define arena_push_many(arena, type, count)         (type *)_arena_push_aligned(arena, sizeof(type), count, alignof(type))
#define arena_push_aligned(arena, type)             (type *)_arena_push_aligned(arena, sizeof(type), 1, alignof(type))
#define arena_push_many_aligned(arena, type, count) (type *)_arena_push_aligned(arena, sizeof(type), count, alignof(type))
#define arena_pop(arena, type)                      (type *)_arena_pop(arena, sizeof(type), 1)
#define arena_pop_many(arena, type, count)          (type *)_arena_pop(arena, sizeof(type), count)

typedef struct {
    char    *beg;
    char    *cur;
    char    *end;
    void    *(*alloc)(usize);   // where a new block comes from when this one is full (NULL: none)
    usize   total;              // every block's bytes, the first included
} Arena;

static inline Arena arena_new(void* (*allocator)(usize), usize size) {
    Arena arena = {};
    
    char *ptr = allocator(size);

#if ARENA_DEBUG
    panic_if_msg(ptr == NULL, 
                 "Arena creation failed: allocator returned a null pointer.");
#endif

    arena.beg = ptr;
    arena.cur = ptr;
    arena.end = ptr + size;
    arena.alloc = allocator;
    arena.total = size;

    return arena;
}

// A push that does not fit starts a NEW block, twice the last or the push itself, whichever is
// larger, and carries on there; the full block is kept, so everything already pushed stays where
// it is (a pointer into the arena is never moved). A fixed block made a large legal program, 4,096
// calls in one function, panic in lowering (exit 134) instead of compiling. A pop or a clear sees
// only the current block, which no caller crosses (a pop undoes the last push). ARENA_MAX_TOTAL
// bounds an arena's blocks together: past it the compiler stops with a message and exit 70, as
// for any out of memory, not with a panic, and never by taking the machine's memory.
#ifndef ARENA_MAX_TOTAL
#define ARENA_MAX_TOTAL ((usize)1 << 30)
#endif
static inline void arena_grow(Arena *arena, isize need) {
    isize cap = arena->end - arena->beg;
    cap = cap > 0 ? 2 * cap : 4096;
    if (cap < need + 4096) cap = need + 4096;
    if (arena->total + (usize)cap > ARENA_MAX_TOTAL) {
        fprintf(stderr, "lain: out of memory: an arena would pass %llu MB (the program is too large for this compiler)\n",
                (unsigned long long)(ARENA_MAX_TOTAL >> 20));
        exit(70);
    }
    arena->total += (usize)cap;
    char *ptr = arena->alloc((usize)cap);
    panic_if_msg(ptr == NULL, "Arena growth failed: the allocator returned a null pointer (%ld bytes).", cap);
    arena->beg = ptr;
    arena->cur = ptr;
    arena->end = ptr + cap;
}

static inline void arena_align(Arena *arena, isize alignment) {
    isize padding = -(uptr)arena->cur & (alignment - 1);

#if ARENA_DEBUG
    isize available = arena->end - arena->cur - padding;
    panic_if_msg(alignment <= 0, 
                 "Arena alignment failed: alignment must be greater than 0 (alignment: %ld).", alignment);
    panic_if_msg(available <= 0, 
                 "Arena alignment failed: insufficient space after padding (available: %ld, required: %ld).", available, padding);
#endif

    arena->cur += padding;
}

static inline void *_arena_push(Arena *arena, isize size, isize count) {
    if (arena->alloc && size > 0 && count > 0 && count > (arena->end - arena->cur) / size) arena_grow(arena, size * count);
#if ARENA_DEBUG
    //fprintf(stderr, "  pushing %zu bytes; %zu bytes remain\n", size*count, (size_t)(arena->end - arena->cur));
    
    panic_if_msg(size <= 0, 
                 "Arena push failed: size must be greater than 0 (got: %ld).", size);
    panic_if_msg(count <= 0, 
                 "Arena push failed: count must be greater than 0 (got: %ld).", count);

    isize available = arena->end - arena->cur;
    panic_if_msg(available <= 0, 
                 "Arena push failed: no space available in the arena (available: %ld bytes).", available);
    panic_if_msg(count > available / size, 
                 "Arena push failed: insufficient space for requested items (count: %ld, available: %ld).", count, available / size);
#endif

    void *ptr = arena->cur;
    arena->cur += size * count;
    return ptr;
}

static inline void *_arena_push_aligned(Arena *arena, isize size, isize count, isize alignment) {
    isize padding = -(uptr)arena->cur & (alignment - 1);
    if (arena->alloc && size > 0 && count > 0 && alignment > 0 &&
        (arena->end - arena->cur - padding <= 0 || count > (arena->end - arena->cur - padding) / size)) {
        arena_grow(arena, size * count + alignment);
        padding = -(uptr)arena->cur & (alignment - 1);
    }

#if ARENA_DEBUG
    //fprintf(stderr, "  pushing %zu bytes; %zu bytes remain\n",size*count, (size_t)(arena->end - arena->cur));

    panic_if_msg(size <= 0, 
                 "Arena push aligned failed: size must be greater than 0 (got: %ld).", size);
    panic_if_msg(count <= 0, 
                 "Arena push aligned failed: count must be greater than 0 (got: %ld).", count);
    panic_if_msg(alignment <= 0, 
                 "Arena push aligned failed: alignment must be greater than 0 (alignment: %ld).", alignment);

    isize available = arena->end - arena->cur - padding;
    panic_if_msg(available <= 0, 
                 "Arena push aligned failed: no space available after alignment (available: %ld bytes).", available);
    panic_if_msg(count > available / size, 
                 "Arena push aligned failed: insufficient space for requested items (count: %ld, available: %ld).", count, available / size);
#endif

    void *ptr = arena->cur + padding;
    arena->cur += padding + size * count;
    return ptr;
}

static inline void *_arena_pop(Arena *arena, isize size, isize count) {
#if ARENA_DEBUG
    panic_if_msg(size <= 0, 
                 "Arena pop failed: size must be greater than 0 (got: %ld).", size);
    panic_if_msg(count <= 0, 
                 "Arena pop failed: count must be greater than 0 (got: %ld).", count);
    panic_if_msg(arena->cur - arena->beg < size * count, 
                 "Arena pop failed: not enough items to pop (requested: %ld items of size %ld, available: %ld bytes).", count, size, arena->cur - arena->beg);
    panic_if_msg(size * count < 0, 
                 "Arena pop failed: overflow detected (size: %ld, count: %ld).", size, count);
#endif

    return arena->cur -= size * count;
}

static inline void arena_clear(Arena *arena) {
    arena->cur = arena->beg;
}

#endif /* UTILS_ARENA_H */