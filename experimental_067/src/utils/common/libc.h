#ifndef LIBC_H
#define LIBC_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "predef.h"  /* LANGUAGE_C99, LANGUAGE_CPP11, OS_WINDOWS, OS_LINUX */

#if OS_WINDOWS
#   include <windows.h>
#elif OS_LINUX
#   include <sys/mman.h>
#   include <sys/stat.h>
#   include <sys/types.h>
#   include <unistd.h>
#   include <fcntl.h>

#endif

#if LANGUAGE_C99 || LANGUAGE_CPP11
#   include <stdbool.h>
#   include <stdint.h>
#endif

#if LANGUAGE_C11
#   include <stdalign.h>
#   include <stdatomic.h>
#   include <stdnoreturn.h>
#   include <threads.h>
#   include <uchar.h>
#endif

#if LANGUAGE_C23
#   include <stdbit.h> 
#   include <stdckdint.h>
#endif

#include <assert.h>
#include <complex.h>
#include <ctype.h>
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <inttypes.h>
#include <iso646.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tgmath.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#endif /* LIBC_H */