/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "arena.h"
#include "common/libc.h"
#include "common/system.h" /* memory_page_alloc, MEMORY_PAGE_MINIMUM_SIZE */
#include "assert.h"
#include "memory_address.h"

void test_arena_push(void)
{
    Arena arena = arena_new(&memory_alloc, MEMORY_PAGE_MINIMUM_SIZE);
    
    int* p;
    
    /* test if the first allocation address is the same as the arena address */
    p = _arena_push(&arena, 8, 1);
    assert((char *)p == arena.beg);

    /* test if the allocation address is 4 bytes further  */
    p = _arena_push(&arena, 4, 1);
    assert((char *)p == (arena.cur - 4));

    /* test the allocation of an array of 3 integers, followed by another element */
    arena_clear(&arena);

    p = arena_push_many(&arena, int, 3);
    p[0] = 11;
    p[1] = 22;
    p[2] = 33;

    assert((char *)&p[0] == arena.beg);
    assert((char *)&p[1] == (arena.beg + 1*sizeof(int)));
    assert((char *)&p[2] == (arena.beg + 2*sizeof(int)));

    p = arena_push(&arena, int);
    assert((char *)p == (arena.beg + 3*sizeof(int)));
}

void test_arena_push_align(void)
{
    Arena arena = arena_new(&memory_alloc, MEMORY_PAGE_MINIMUM_SIZE);

    int *p;

    /* test if the the arena pointer is page aligned */
    assert(memory_address_is_aligned(arena.beg, MEMORY_PAGE_MINIMUM_SIZE));

    /* test the allocation of an array of 3 integers, followed by another element  */
    arena_clear(&arena);

    p = arena_push_many_aligned(&arena, int, 3);
    p[0] = 11;
    p[1] = 22;
    p[2] = 33;

    assert((char *)&p[0] == arena.beg);
    assert((char *)&p[1] == (arena.beg + 1*sizeof(int)));
    assert((char *)&p[2] == (arena.beg + 2*sizeof(int)));

    p = arena_push(&arena, int);
    assert((char *)p == (arena.beg + 3*sizeof(int)));

    /* test correct alignment calculaton */
    arena_clear(&arena);
    
    p = _arena_push_aligned(&arena, 2, 1, 8);
    p = _arena_push_aligned(&arena, 4, 1, 8);

    assert((char *)p == (arena.beg + 2 + 6));

    /* test 1 alignment */
    arena_clear(&arena);

    p = _arena_push_aligned(&arena, 2, 1, 1);
    assert((char *)p == arena.beg);
    p = (int*)_arena_push_aligned(&arena, 4, 1, 1);
    assert((char *)p == (arena.beg + 2));
}

int main(void)
{
    test_arena_push();
    test_arena_push_align();
    printf("All test passed succesfully\n");
}