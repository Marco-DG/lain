#undef MEMORY_PAGE_DEBUG
#include "../../memory_address.h"
#include "memory.h"

void test_memory_alloc(void)
{
    /* test if allocating 0 bytes, always result in failure */
    assert(memory_alloc(0) == MEMORY_PAGE_ALLOC_FAILED);

    /* test if allocating size_t max, always result in failure,
    it should not though, cause both mmap and VirtualAlloc should
    take in a size_t */
    assert(memory_alloc((size_t) -1) == MEMORY_PAGE_ALLOC_FAILED);

    /* test if the allocation is aligned to MEMORY_PAGE_MINIMUM_SIZE */
    assert(memory_address_is_aligned(memory_alloc(16), MEMORY_PAGE_MINIMUM_SIZE));
}

void test_memory_free(void)
{
    /* allocation to test */
    void* ptr = memory_alloc(16);
    assert(ptr != MEMORY_PAGE_ALLOC_FAILED);

    /* test if freeing a 0 size, always result in failure,
    in Linux it does, in Windows size is discarded */
    /*assert_equal(memory_free(ptr, 0), MEMORY_PAGE_FREE_FAILED);*/

    /* test if correct handling is successful */
    assert(memory_free(ptr, 16) != MEMORY_PAGE_FREE_FAILED);
}

void test_memory_page_size(void)
{
    isize page_size = memory_page_size();
    printf("page_size: %li\n", page_size);

    /* test if memory_size() returns a number greater or equal than MEMORY_PAGE_MINIMUM_SIZE */
    assert(page_size >= MEMORY_PAGE_MINIMUM_SIZE);
}

int main(void)
{
    test_memory_alloc();
    test_memory_free();
    test_memory_page_size();
}