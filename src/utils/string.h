#ifndef UTILS_STRING_H
#define UTILS_STRING_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    ref: https://nullprogram.com/blog/2023/09/30/
*/

#include "common/def.h"  /* uint8, countof */
#include "common/libc.h" /* memcmp */

typedef struct {
    uint8 *data;
    isize len;
} string;

#define lengthof(s) (countof(s) - 1)
#define string_new(s) { (uint8 *) s, lengthof(s) }

static uint64 string_hash(string s)
{
    uint64_t h = 0x100;
    for (ptrdiff_t i = 0; i < s.len; i++)
    {
        h ^= s.data[i];
        h *= 1111111111111111111u;
    }
    return h;
}

static inline bool string_equals(string a, string b) {
    return a.len == b.len && !memcmp(a.data, b.data, a.len);
}

#endif /* UTILS_STRING_H */