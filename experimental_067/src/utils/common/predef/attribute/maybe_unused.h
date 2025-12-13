#ifndef ATTRIBUTE_MAYBE_UNUSED_H
#define ATTRIBUTE_MAYBE_UNUSED_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../language/c.h"
#include "../language/cpp.h"
#include "../compiler/feature/has_attribute.h"

#if LANGUAGE_CPP17 || LANGUAGE_C23
#   define ATTRIBUTE_MAYBE_UNUSED [[maybe_unused]]

#elif COMPILER_HAS_ATTRIBUTE(unused)
#   define ATTRIBUTE_MAYBE_UNUSED __attribute__((unused))

#else
#   define ATTRIBUTE_MAYBE_UNUSED

#endif

#endif /* ATTRIBUTE_MAYBE_UNUSED_H */