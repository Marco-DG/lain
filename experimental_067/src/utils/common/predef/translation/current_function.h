#ifndef TRANSLATION_CURRENT_FUNCTION_H
#define TRANSLATION_CURRENT_FUNCTION_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#if defined(__GNUC__) || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) || (defined(__ICC) && (__ICC >= 600)) || defined(__ghs__) || defined(__clang__)
#   define TRANSLATION_CURRENT_FUNCTION __PRETTY_FUNCTION__

#elif defined(__DMC__) && (__DMC__ >= 0x810)
#   define TRANSLATION_CURRENT_FUNCTION __PRETTY_FUNCTION__

#elif defined(__FUNCSIG__)
#   define TRANSLATION_CURRENT_FUNCTION __FUNCSIG__

#elif (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 600)) || (defined(__IBMCPP__) && (__IBMCPP__ >= 500))
#   define TRANSLATION_CURRENT_FUNCTION __FUNCTION__

#elif defined(__BORLANDC__) && (__BORLANDC__ >= 0x550)
#   define TRANSLATION_CURRENT_FUNCTION __FUNC__

#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
#   define TRANSLATION_CURRENT_FUNCTION __func__

#elif defined(__cplusplus) && (__cplusplus >= 201103)
#   define TRANSLATION_CURRENT_FUNCTION __func__

#else
#   define TRANSLATION_CURRENT_FUNCTION "(unknown)"

#endif

#endif /* TRANSLATION_CURRENT_FUNCTION_H */