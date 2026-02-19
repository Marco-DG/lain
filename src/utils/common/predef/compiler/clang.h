#ifndef COMPILER_CLANG_H
#define COMPILER_CLANG_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from:
        https://github.com/nemequ/hedley/tree/master
*/

#include "feature/version.h"

#if defined(__clang__)
#   define COMPILER_CLANG_ENCODED_VERSION COMPILER_VERSION_ENCODE(__clang_major__, __clang_minor__, __clang_patchlevel__)

#endif

#ifdef COMPILER_CLANG_ENCODED_VERSION
#   define COMPILER_CLANG 1
#   define COMPILER_CLANG_VERSION(major, minor, patch) (COMPILER_CLANG_ENCODED_VERSION >= COMPILER_VERSION_ENCODE(major, minor, patch))

#else
#   define COMPILER_CLANG 0
#   define COMPILER_CLANG_VERSION(major, minor, patch) (0)

#endif

#endif /* COMPILER_CLANG_H */