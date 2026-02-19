#ifndef COMPILER_IBM_H
#define COMPILER_IBM_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from:
        https://github.com/nemequ/hedley/tree/master
*/

#include "feature/version.h"

#if defined(__ibmxl__)
#   define COMPILER_IBM_ENCODED_VERSION COMPILER_VERSION_ENCODE(__ibmxl_version__, __ibmxl_release__, __ibmxl_modification__)

#elif defined(__xlC__) && defined(__xlC_ver__)
#   define COMPILER_IBM_ENCODED_VERSION COMPILER_VERSION_ENCODE(__xlC__ >> 8, __xlC__ & 0xff, (__xlC_ver__ >> 8) & 0xff)

#elif defined(__xlC__)
#   define COMPILER_IBM_ENCODED_VERSION COMPILER_VERSION_ENCODE(__xlC__ >> 8, __xlC__ & 0xff, 0)

#endif

#ifdef COMPILER_IBM_ENCODED_VERSION
#   define COMPILER_IBM 1
#   define COMPILER_IBM_VERSION(major, minor, patch) (COMPILER_IBM_ENCODED_VERSION >= COMPILER_VERSION_ENCODE(major, minor, patch))

#else
#   define COMPILER_IBM 0
#   define COMPILER_IBM_VERSION(major, minor, patch) (0)

#endif

#endif /* COMPILER_IBM_H */