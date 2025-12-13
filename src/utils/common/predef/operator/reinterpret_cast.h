#ifndef OPERATOR_REINTERPRET_CAST_H
#define OPERATOR_REINTERPRET_CAST_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "../language/cpp.h"

#if LANGUAGE_CPP
#   define OPERATOR_REINTERPRET_CAST(T, expr) (reinterpret_cast<T>(expr))

#else
#   define OPERATOR_REINTERPRET_CAST(T, expr) ((T) (expr))

#endif

#endif /* OPERATOR_REINTERPRET_CAST_H */