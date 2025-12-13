#ifndef ATTRIBUTE_NODISCARD_H
#define ATTRIBUTE_NODISCARD_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../language/c.h"
#include "../language/cpp.h"
#include "../compiler/feature/has_attribute.h"

#if LANGUAGE_CPP17 || LANGUAGE_C23
#   define ATTRIBUTE_NODISCARD          [[nodiscard]]
#   define ATTRIBUTE_NODISCARD_MSG(msg) [[nodiscard(msg)]]

#elif COMPILER_HAS_ATTRIBUTE(warn_unused_result)
#   define ATTRIBUTE_NODISCARD          __attribute__((__warn_unused_result__))
#   define ATTRIBUTE_NODISCARD_MSG(msg) __attribute__((__warn_unused_result__))

#elif defined(_Check_return_) /* Visual C++ SAL */
#   define ATTRIBUTE_NODISCARD          _Check_return_
#   define ATTRIBUTE_NODISCARD_MSG(msg) _Check_return_

#else
#   define ATTRIBUTE_NODISCARD

#endif

#endif /* ATTRIBUTE_NODISCARD_H */ 