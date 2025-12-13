#ifndef BUILTIN_LIKELY_H
#define BUILTIN_LIKELY_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_builtin.h"

#if COMPILER_HAS_BUILTIN(__builtin_expect)
#   define BUILTIN_LIKELY(expr) __builtin_expect(!!(expr), 1)

#else
#   define BUILTIN_LIKELY(expr) (!!(expr))

#endif

#endif /* BUILTIN_LIKELY_H */