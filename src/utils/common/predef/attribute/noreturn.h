#ifndef ATTRIBUTE_NORETURN_H
#define ATTRIBUTE_NORETURN_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../language/c.h"
#include "../language/cpp.h"
#include "../compiler/feature/has_attribute.h"

#if LANGUAGE_CPP11 || LANGUAGE_C23
#   define ATTRIBUTE_NORETURN [[noreturn]]

#elif LANGUAGE_C11
#   define ATTRIBUTE_NORETURN _Noreturn

#elif COMPILER_HAS_ATTRIBUTE(noreturn)
#   define ATTRIBUTE_NORETURN __attribute__((__noreturn__))

#else
#   define ATTRIBUTE_NORETURN

#endif

#endif /* ATTRIBUTE_NORETURN_H */