#ifndef LANGUAGE_C_H
#define LANGUAGE_C_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#define LANGUAGE_C       0
#define LANGUAGE_C23     0
#define LANGUAGE_C17     0
#define LANGUAGE_C11     0
#define LANGUAGE_C99     0

#if defined(__STDC__)
#   undef  LANGUAGE_C
#   define LANGUAGE_C 1

#   if (__STDC_VERSION__ >= 202311L)
#      undef  LANGUAGE_C23
#      define LANGUAGE_C23 1
#   endif

#   if (__STDC_VERSION__ >= 201710L)
#      undef  LANGUAGE_C17
#      define LANGUAGE_C17 1
#   endif

#   if (__STDC_VERSION__ >= 201112L)
#      undef  LANGUAGE_C11
#      define LANGUAGE_C11 1
#   endif

#   if (__STDC_VERSION__ >= 199901L)
#      undef  LANGUAGE_C99
#      define LANGUAGE_C99 1
#   endif

#endif

#endif /* LANGUAGE_C_H */