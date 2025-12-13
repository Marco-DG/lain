#ifndef ATTRIBUTE_NOTHROW_H
#define ATTRIBUTE_NOTHROW_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(nothrow)
#   define ATTRIBUTE_NOTHROW __attribute__((__nothrow__))

#else
#   define ATTRIBUTE_NOTHROW

#endif

#endif /* ATTRIBUTE_NOTHROW_H */