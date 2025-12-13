#ifndef ATTRIBUTE_PURE_H
#define ATTRIBUTE_PURE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(pure)
#   define ATTRIBUTE_PURE __attribute__((__pure__))

#else
#   define ATTRIBUTE_PURE

#endif

#endif /* ATTRIBUTE_PURE_H */