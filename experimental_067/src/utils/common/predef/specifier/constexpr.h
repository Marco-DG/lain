#ifndef ATTRIBUTE_CONSTEXPR_H
#define ATTRIBUTE_CONSTEXPR_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../language/c.h"
#include "../language/cpp.h"

#if LANGUAGE_CPP || LANGUAGE_C23
#   define ATTRIBUTE_CONSTEXPR constexpr

#else
#   define ATTRIBUTE_CONSTEXPR

#endif

#endif /* ATTRIBUTE_CONSTEXPR_H */