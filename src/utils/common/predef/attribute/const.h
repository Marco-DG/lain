#ifndef ATTRIBUTE_CONST_H
#define ATTRIBUTE_CONST_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(const)
#   define ATTRIBUTE_CONST __attribute__((__const__))

#else
#   define ATTRIBUTE_CONST

#endif

#endif /* ATTRIBUTE_CONST_H */