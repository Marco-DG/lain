/* Copyright © 2024 Marco De Groskovskaja, licensed under the MIT License, see https://mit-license.org for details. */

#ifndef UTILS_SCRATCH_ARENA_H
#define UTILS_SCRATCH_ARENA_H

/*
    from: https://nullprogram.com/blog/2023/09/27/
*/

#include "common/def.h"     /* usize, isize, uptr */
#include "common/predef.h"  /* nodiscard, inline, alignof */
#include "panic.h"          /* panic_if_msg */

#ifndef ARENA_DEBUG
#define ARENA_DEBUG 1
#endif

#define arena_push(arena, type)                     (type *)_arena_push(arena, sizeof(type), 1)
#define arena_push_many(arena, type, count)         (type *)_arena_push(arena, sizeof(type), count)
#define arena_push_aligned(arena, type)             (type *)_arena_push_aligned(arena, sizeof(type), 1, alignof(type))
#define arena_push_many_aligned(arena, type, count) (type *)_arena_push_aligned(arena, sizeof(type), count, alignof(type))

typedef struct {
    char    *cur;
    char    *end;
} Arena;

static inline Arena arena_new(void* (*allocator)(usize), usize size) {
    Arena arena = {0};
    
    char *ptr = allocator(size);

#if ARENA_DEBUG
    panic_if_msg(ptr == NULL, 
                 "Arena creation failed: allocator returned a null pointer.");
#endif

    arena.cur = ptr;
    arena.end = ptr + size;

    return arena;
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
#if ARENA_DEBUG
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

#if ARENA_DEBUG
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

#endif /* UTILS_SCRATCH_ARENA_H */