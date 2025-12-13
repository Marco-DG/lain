#ifndef ATTRIBUTE_RESTRICT_H
#define ATTRIBUTE_RESTRICT_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
    
    from: https://github.com/nemequ/hedley/blob/master/hedley.h
    todo: more compilers.
    todo: test compilers on godbolt, and write comment like / tested on godbolt.org /
*/

#include "../language/c.h"
#include "../language/cpp.h"
#include "../compiler/clang.h"
#include "../compiler/gcc.h"
#include "../compiler/ibm.h"
#include "../compiler/msvc.h"
#include "../compiler/feature/has_attribute.h"

#if LANGUAGE_C99 && !LANGUAGE_CPP
#   define ATTRIBUTE_RESTRICT restrict

#elif COMPILER_CLANG || COMPILER_GCC_VERSION(3, 1, 0) || COMPILER_MSVC_VERSION(14, 0, 0) ||  COMPILER_IBM_VERSION(10, 1, 0)
#   define ATTRIBUTE_RESTRICT __restrict

#else
#   define ATTRIBUTE_RESTRICT

#endif

#endif /* ATTRIBUTE_RESTRICT_H */