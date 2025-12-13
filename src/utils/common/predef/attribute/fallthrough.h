#ifndef ATTRIBUTE_FALLTHROUGH_H
#define ATTRIBUTE_FALLTHROUGH_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../compiler/feature/has_attribute.h"

#if COMPILER_HAS_ATTRIBUTE(fallthrough)
#   define ATTRIBUTE_FALLTHROUGH __attribute__((__fallthrough__))

#elif defined(__fallthrough) /* Visual C++ SAL */
#   define HEDY_FALLTHROUGH __fallthrough

#else
#   define ATTRIBUTE_FALLTHROUGH

#endif

#endif /* ATTRIBUTE_FALLTHROUGH_H */