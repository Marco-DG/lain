#ifndef LANGUAGE_CPP_H
#define LANGUAGE_CPP_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#define LANGUAGE_CPP     0
#define LANGUAGE_CPP20   0
#define LANGUAGE_CPP17   0
#define LANGUAGE_CPP14   0
#define LANGUAGE_CPP11   0
#define LANGUAGE_CPP98   0

#if defined(__cplusplus)
#   undef  LANGUAGE_CPP
#   define LANGUAGE_CPP 1

#   if (__cplusplus >= 202002L)
#      undef  LANGUAGE_CPP20
#      define LANGUAGE_CPP20 1
#   endif

#   if (__cplusplus >= 201703L)
#      undef  LANGUAGE_CPP17
#      define LANGUAGE_CPP17 1
#   endif

#   if (__cplusplus >= 201402L)
#      undef  LANGUAGE_CPP14
#      define LANGUAGE_CPP14 1
#   endif

#   if (__cplusplus >= 201103L)
#      undef  LANGUAGE_CPP11
#      define LANGUAGE_CPP11 1
#   endif

#   if (__cplusplus >= 199711L)
#      undef  LANGUAGE_CPP98
#      define LANGUAGE_CPP98 1
#   endif

#endif

#endif /* LANGUAGE_CPP_H */