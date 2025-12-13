#ifndef DEF_H
#define DEF_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "libc.h"
#include "predef.h" /* LANGUAGE_C11, OPERATOR_ALIGNOF */

//#if LANGUAGE_C11
//typedef char16_t    char16;
//#endif

typedef float       float32;
typedef double      float64;

typedef int8_t      int8;
typedef int16_t     int16;
typedef int32_t     int32;
typedef int64_t     int64;

typedef uint8_t     uint8;
typedef uint16_t    uint16;
typedef uint32_t    uint32;
typedef uint64_t    uint64;

typedef ptrdiff_t   isize;
typedef size_t      usize;

typedef intptr_t    iptr;
typedef uintptr_t   uptr;

//typedef uint8       byte;

#undef  alignof
#define sizeof(x)       (isize)sizeof(x)
#define alignof(x)      (isize)OPERATOR_ALIGNOF(x)
#define countof(a)      (sizeof(a) / sizeof(*(a)))

//typedef struct {
//    uint8 *data;
//    isize len;
//} uint8_array;
//
//typedef uint8* uint8_stream; // null terminated

#endif /* DEF_H */