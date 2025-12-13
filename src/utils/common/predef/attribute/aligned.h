#ifndef ATTRIBUTE_ALIGNED_H
#define ATTRIBUTE_ALIGNED_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
    
    from: https://github.com/simd-everywhere/simde/blob/master/simde/simde-align.h
*/

#include "../compiler/feature/has_attribute.h"
#include "../language/c.h"
#include "../language/cpp.h"
#include "../compiler/clang.h"
#include "../compiler/gcc.h"

#if COMPILER_HAS_ATTRIBUTE(aligned)
#   define ATTRIBUTE_ALIGNED(alignment) __attribute__((__aligned__(alignment)))

#elif LANGUAGE_CPP11 || LANGUAGE_C23
#   define ATTRIBUTE_ALIGNED(alignment) alignas(alignment)

#elif LANGUAGE_C11 || COMPILER_GCC_VERSION(4,7,3) || COMPILER_CLANG_VERSION(10,0,0) /* tested on godbolt.org */
#   define ATTRIBUTE_ALIGNED(alignment) _Alignas(alignment)

#else
#   define ATTRIBUTE_ALIGNED

#endif

#endif /* ATTRIBUTE_ALIGNED_H */