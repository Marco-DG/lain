#ifndef ATTRIBUTE_NONNULL_H
#define ATTRIBUTE_NONNULL_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(nonnull)
#   define ATTRIBUTE_NONNULL(...) __attribute__((__nonnull__(__VA_ARGS__)))

#else
#  define ATTRIBUTE_NONNULL(...) 

#endif

#endif /* ATTRIBUTE_NONNULL_H */