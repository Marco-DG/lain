#ifndef ATTRIBUTE_INLINE_H
#define ATTRIBUTE_INLINE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../language/c.h"
#include "../language/cpp.h"

#if LANGUAGE_CPP98 || LANGUAGE_C99
#   define ATTRIBUTE_INLINE inline

#else
#   define ATTRIBUTE_INLINE

#endif

#endif /* ATTRIBUTE_INLINE_H */