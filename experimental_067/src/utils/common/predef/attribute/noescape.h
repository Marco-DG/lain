#ifndef ATTRIBUTE_NOESCAPE_H
#define ATTRIBUTE_NOESCAPE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(noescape)
#   define ATTRIBUTE_NOESCAPE __attribute__((__noescape__))

#else
#   define ATTRIBUTE_NOESCAPE

#endif

#endif /* ATTRIBUTE_NOESCAPE_H */