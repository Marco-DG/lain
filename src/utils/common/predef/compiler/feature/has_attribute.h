#ifndef COMPILER_HAS_ATTRIBUTE_H
#define COMPILER_HAS_ATTRIBUTE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#if defined(__has_attribute)
#   define COMPILER_HAS_ATTRIBUTE(attr) __has_attribute(attr)

#else
#   define COMPILER_HAS_ATTRIBUTE(attr) (0)

#endif

#endif /* COMPILER_HAS_ATTRIBUTE_H */