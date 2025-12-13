#ifndef OPERATOR_ALIGNOF_H
#define OPERATOR_ALIGNOF_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

       todo: expand it following https://github.com/simd-everywhere/simde/blob/master/simde/simde-align.h
*/

#include "../language/c.h"
#include "../language/cpp.h"
#include "../compiler/clang.h"
#include "../compiler/gcc.h"

#if LANGUAGE_CPP11 || LANGUAGE_C23
#   define OPERATOR_ALIGNOF(type) alignof(type)

#elif LANGUAGE_C11 || COMPILER_GCC_VERSION(4,7,3) || COMPILER_CLANG_VERSION(10,0,0) /* tested on godbolt.org */
#   define OPERATOR_ALIGNOF(type) _Alignof(type)

#else
#   warning "Unsupported operator: alignof"

#endif

#endif /* OPERATOR_ALIGNOF_H */