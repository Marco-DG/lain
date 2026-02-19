#ifndef COMPILER_HAS_BUILTIN_H
#define COMPILER_HAS_BUILTIN_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#if defined(__has_builtin)
#   define COMPILER_HAS_BUILTIN(builtin) __has_builtin(builtin)

#else
#   define COMPILER_HAS_BUILTIN(builtin) (0)

#endif

#endif /* COMPILER_HAS_BUILTIN_H */