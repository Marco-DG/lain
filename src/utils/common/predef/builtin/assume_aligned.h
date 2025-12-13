#ifndef BUILTIN_ASSUME_ALIGNED_H
#define BUILTIN_ASSUME_ALIGNED_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
    
    from: https://github.com/simd-everywhere/simde/blob/master/simde/simde-align.h
    todo: expand it from https://github.com/simd-everywhere/simde/blob/master/simde/simde-align.h
*/

#include "../compiler/feature/has_builtin.h"
#include "../compiler/gcc.h"
#include "../language/cpp.h"

#if COMPILER_HAS_BUILTIN(__builtin_assume_aligned) || COMPILER_GCC_VERSION(4,7,0) /* tested 4.7.1 on godbolt.org */
#   define BUILTIN_ASSUME_ALIGNED(pointer, alignment) __builtin_assume_aligned(pointer, alignment)

#elif LANGUAGE_CPP17
#   include "memory.h"
#   std::assume_aligned<alignment>(pointer)

#else
#   define BUILTIN_ASSUME_ALIGNED(pointer, alignment) (alignment)

#endif

#endif /* BUILTIN_ASSUME_ALIGNED_H */