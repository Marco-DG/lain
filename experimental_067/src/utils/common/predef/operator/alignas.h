#ifndef OPERATOR_ALIGNAS_H
#define OPERATOR_ALIGNAS_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../language/c.h"
#include "../language/cpp.h"
#include "../compiler/clang.h"
#include "../compiler/gcc.h"

#if LANGUAGE_CPP11 || LANGUAGE_C23
#   define OPERATOR_ALIGNAS(expr_or_type) alignas(expr_or_type)

#elif LANGUAGE_C11 || COMPILER_GCC_VERSION(4,7,3) || COMPILER_CLANG_VERSION(10,0,0) /* tested on godbolt.org */
#   define OPERATOR_ALIGNAS(expr_or_type) _Alignas(expr_or_type)

#else
#   warning "Unsupported operator: alignas"

#endif

#endif /* OPERATOR_ALIGNAS_H */