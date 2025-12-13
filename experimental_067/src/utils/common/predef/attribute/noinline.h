#ifndef ATTRIBUTE_NOINLINE_H
#define ATTRIBUTE_NOINLINE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(noinline)
#   define ATTRIBUTE_NOINLINE __attribute__((__noinline__))

#else
#   define ATTRIBUTE_NOINLINE

#endif

#endif /* ATTRIBUTE_NOINLINE_H */