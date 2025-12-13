#ifndef BUILTIN_EXPECT_H
#define BUILTIN_EXPECT_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_builtin.h"

#if COMPILER_HAS_BUILTIN(__builtin_expect)
#   define BUILTIN_EXPECT(expr, value) __builtin_expect((expr), value)

#else
#   define BUILTIN_EXPECT(expr, value) (expr)

#endif

#endif /* BUILTIN_EXPECT_H */