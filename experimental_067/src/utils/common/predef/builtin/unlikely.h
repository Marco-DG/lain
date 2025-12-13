#ifndef BUILTIN_UNLIKELY_H
#define BUILTIN_UNLIKELY_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_builtin.h"

#if COMPILER_HAS_BUILTIN(__builtin_expect)
#   define BUILTIN_UNLIKELY(expr) __builtin_expect(!!(expr), 0)

#else
#   define BUILTIN_UNLIKELY(expr) (!!(expr))

#endif

#endif /* BUILTIN_UNLIKELY_H */