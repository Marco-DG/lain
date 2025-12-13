#ifndef ATTRIBUTE_MALLOC_H
#define ATTRIBUTE_MALLOC_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(malloc)
#   define ATTRIBUTE_MALLOC __attribute__((__malloc__))

#else
#   define ATTRIBUTE_MALLOC

#endif

#endif /* ATTRIBUTE_MALLOC_H */