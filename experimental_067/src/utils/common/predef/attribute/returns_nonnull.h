#ifndef ATTRIBUTE_RETURNS_NONNULL_H
#define ATTRIBUTE_RETURNS_NONNULL_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(returns_nonnull)
#   define ATTRIBUTE_RETURNS_NONNULL __attribute__((__returns_nonnull__))

#elif defined(_Ret_notnull_) /* Visual C++ SAL */
#   define ATTRIBUTE_RETURNS_NONNULL _Ret_notnull_

#else
#   define ATTRIBUTE_RETURNS_NONNULL

#endif

#endif /* ATTRIBUTE_RETURNS_NONNULL_H */