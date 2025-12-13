/* Copyright © 2024 Marco De Groskovskaja, licensed under the MIT License, see https://mit-license.org for details. */

#ifndef SYSTEM_MEMORY_H
#define SYSTEM_MEMORY_H

#include "../def.h"     /* usize, isize */
#include "../libc.h"    /* <windows.h>, <sys/mman.h>, NULL */
#include "../predef.h"  /* inline, nodiscard, maybe_unused, OS_WINDOWS, OS_LINUX */

/**
    @brief
        Toggle to enable runtime checks
*/
#ifndef MEMORY_DEBUG
#define MEMORY_DEBUG 0
#endif

///**
//    @brief
//        Size (in bytes) of the virtual address space
//*/
//#undef MEMORY_VIRTUAL_ADDRESS_SPACE_SIZE

/**
    @brief
        Minimum guaranteed size (in bytes) of the allocated page
        and also minimum guaranteed page alignment.
*/
#undef MEMORY_PAGE_MINIMUM_SIZE

/**
    @brief
        Value returned by `memory_reserve` on failure.
*/
//#undef MEMORY_RESERVE_FAILED

/**
    @brief
        Value returned by `memory_commit` on failure.
*/
//#undef MEMORY_COMMIT_FAILED

/**
    @brief
        Value returned by `memory_alloc` on failure.
*/
#undef MEMORY_ALLOC_FAILED

/**
    @brief
        Value returned by `memory_free` on failure.
*/
#undef MEMORY_FREE_FAILED

/**
    @brief
        Reserves a range of the process's virtual address space.

        You can commit reserved pages in subsequent calls to
        `memory_commit`.

    @param size
        Size of memory to reserve, in bytes.

    @return
        A pointer to the reserved memory on success.
        `MEMORY_RESERVE_FAILED` on failure.
*/
//static void *memory_reserve(isize size);

/**
    @brief
        Commits previously reserved memory.

    @param addr
        Non-null pointer to the reserved memory block to commit.
    @param size
        Size of memory to commit, in bytes.

    @return
        `MEMORY_COMMIT_FAILED` on failure.
*/
//static int memory_commit(void* addr, isize size);

/**
    @brief
        The allocated memory size is rounded up to the minimum page size
        by the OS' API, thus it will be greater or equal to `size`.

        4 KiB is the minimum page size for most architectures.

        Remember to free the allocated page with `memory_free`.
    @param size
        Memory allocation size, in bytes.
    @return
        Page-aligned pointer to the allocated memory region on success,
        `MEMORY_ALLOC_FAILED` on failure.
*/
nodiscard static inline noalias void *memory_alloc(usize size);

/**
    @brief
        Frees memory allocated by memory_page_alloc function.
    @param ptr
        Pointer to the memory block to be freed.
    @param size
        Size of the memory block to be freed (unused on Windows).
    @return
        `MEMORY_FREE_FAILED` on failure.
*/
static inline nonnull(1) int memory_free(noescape void *ptr, maybe_unused usize size);

/**
    @return
        The system's memory page size
*/
nodiscard static inline int64 memory_page_size(void);


//#if WORDSIZE_BITS == 32
//#   define MEMORY_VIRTUAL_ADDRESS_SPACE_SIZE 0xFFFFFFFF /* 32 bits */
//
//#elif WORDSIZE_BITS == 64
//#   define MEMORY_VIRTUAL_ADDRESS_SPACE_SIZE 0xFFFFFFFFFFFF /* 48 bits */
//
//#endif

/* TODO: https://ziglang.org/documentation/master/std/#std.mem.page_size */
#if ARCH_SPARC_V9
#   define MEMORY_PAGE_MINIMUM_SIZE     8*1024

#else
#   define MEMORY_PAGE_MINIMUM_SIZE     4*1024

#endif

#if OS_WINDOWS
//#   define MEMORY_PAGE_RESERVE_FAILED   NULL
//#   define MEMORY_PAGE_COMMIT_FAILED    NULL
#   define MEMORY_PAGE_ALLOC_FAILED     NULL
#   define MEMORY_PAGE_FREE_FAILED      0

#elif OS_LINUX
//#   define MEMORY_PAGE_RESERVE_FAILED   MAP_FAILED
//#   define MEMORY_PAGE_COMMIT_FAILED    -1
#   define MEMORY_PAGE_ALLOC_FAILED     MAP_FAILED
#   define MEMORY_PAGE_FREE_FAILED      -1

#endif

//static inline noalias void *memory_reserve(isize size)
//{
//    void *p;
//
//#if OS_WINDOWS
//    p = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
//
//#elif OS_LINUX
//    p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
//
//#endif
//
//#if MEMORY_PAGE_DEBUG
//    assert(p != MEMORY_PAGE_RESERVE_FAILED);
//
//#endif
//
//    return p;
//}
//
///*
//    issue: this function doesn't validate whether the addr is indeed part of a previously reserved region. Accessing invalid memory regions may lead to undefined behavior.
//*/
//static inline nonnull(1) int memory_commit(void* addr, isize size)
//{
//    int i;
//
//#if OS_WINDOWS
//    i = VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
//
//#elif OS_LINUX
//    i = mprotect(addr, size, PROT_READ|PROT_WRITE);
//
//#endif
//
//#if MEMORY_PAGE_DEBUG
//    assert(i != MEMORY_PAGE_COMMIT_FAILED);
//
//#endif
//
//    return i;
//}

nodiscard static inline noalias void *memory_alloc(usize size)
{
    void* p;

#if OS_WINDOWS
    //  If the lpAddress parameter is NULL, dwSize is rounded up to the next page boundary
    p = VirtualAlloc(NULL, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    /*
        memory_alloc PROC
                mov     rdx, rcx
                mov     r9d, 4
                xor     ecx, ecx
                mov     r8d, 12288
                rex_jmp QWORD PTR __imp_VirtualAlloc
    */

#elif OS_LINUX
    /* TODO: analyze the cost/benefit of hinting mmap as in:
    https://ziglang.org/documentation/master/std/#src/std/heap/PageAllocator.zig */
    p = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    /*
        memory_alloc:
                mov     rsi, rdi
                xor     r9d, r9d
                mov     r8d, -1             # __fd
                mov     ecx, 34             # MAP_PRIVATE|MAP_ANONYMOUS
                mov     edx, 3              # PROT_READ|PROT_WRITE
                xor     edi, edi
                jmp     mmap
    */

#endif

#if MEMORY_PAGE_DEBUG
    assert(p != MEMORY_PAGE_ALLOC_FAILED);

#endif

    return p;
}

static inline nonnull(1) int memory_free(noescape void *ptr, maybe_unused usize size)
{
    int i;

#if OS_WINDOWS
    i = VirtualFree(ptr, 0, MEM_RELEASE); /* BOOL */
    /*
        memory_free PROC
                xor     edx, edx
                mov     r8d, 32768
                rex_jmp QWORD PTR __imp_VirtualFree
    */

#elif OS_LINUX
    i = munmap(ptr, size); /* int */
    /*
        memory_free:
                jmp     munmap@PLT
    */

#endif

#if MEMORY_PAGE_DEBUG
    assert(i != MEMORY_PAGE_FREE_FAILED);

#endif

    return i;
}

nodiscard static inline int64 memory_page_size(void)
{
#if OS_WINDOWS
    SYSTEM_INFO SystemInfo;
    GetSystemInfo(&SystemInfo);
    return SystemInfo.dwAllocationGranularity; /* DWORD - uint32 */
    /*
        memory_page_size PROC
        $LN4:
                sub     rsp, 88
                lea     rcx, QWORD PTR SystemInfo$[rsp]
                call    QWORD PTR __imp_GetSystemInfo
                mov     eax, DWORD PTR SystemInfo$[rsp+40]
                add     rsp, 88
                ret     0
    */

#elif OS_LINUX
    return sysconf(_SC_PAGE_SIZE); /* long - int64 */
    /*
        memory_page_size:
                mov     edi, 30             #_SC_PAGE_SIZE
                jmp     sysconf@PLT
    */

#endif
}

#endif /* SYSTEM_MEMORY_H */