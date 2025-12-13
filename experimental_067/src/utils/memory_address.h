/* Copyright © 2024 Marco De Groskovskaja, licensed under the MIT License, see https://mit-license.org for details. */

#ifndef MEMORY_ADDRESS_H
#define MEMORY_ADDRESS_H

#include "common/libc.h"    /* bool, uintptr_t */
#include "common/predef.h"  /* inline, nodiscard, nonnull */

/*
    from: https://stackoverflow.com/a/1898487/8411453

    The conversion foo * -> void * might involve an actual computation,
    eg adding an offset. The standard also leaves it up to the
    implementation what happens when converting (arbitrary) pointers to
    integers, but I suspect that it is often implemented as a noop.

    For such an implementation, foo * -> uintptr_t -> foo * would work,
    but foo * -> uintptr_t -> void * and void * -> uintptr_t -> foo *
    wouldn't. The alignment computation would also not work reliably
    because you only check alignment relative to the segment offset,
    which might or might not be what you want.

    In conclusion: Always use void * to get implementation-independant
    behaviour.
*/
nodiscard static nonnull(1) inline bool memory_address_is_aligned(void *ptr, size_t bytes)
{
    return ((uintptr_t)ptr % bytes) == 0;
}

nodiscard static nonnull(1) inline bool memory_address_is_aligned_log2(void *ptr, size_t bytes_log2)
{
    return ((uintptr_t)ptr & (bytes_log2 -1)) == 0;
}

#endif /* MEMORY_ADDRESS_H */