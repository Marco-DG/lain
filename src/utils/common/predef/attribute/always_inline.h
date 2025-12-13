#ifndef ATTRIBUTE_ALWAYS_INLINE_H
#define ATTRIBUTE_ALWAYS_INLINE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(always_inline)
#   define ATTRIBUTE_ALWAYS_INLINE __attribute__((__always_inline__))

#else
#   define ATTRIBUTE_ALWAYS_INLINE

#endif

#endif /* ATTRIBUTE_ALWAYS_INLINE_H */