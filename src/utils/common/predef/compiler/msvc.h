#ifndef COMPILER_MSVC_H
#define COMPILER_MSVC_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from:
        https://github.com/nemequ/hedley/tree/master
*/

#include "feature/version.h"

#if defined(_MSC_FULL_VER) && (_MSC_FULL_VER >= 140000000) && !defined(__ICL)
#  define COMPILER_MSVC_ENCODED_VERSION COMPILER_VERSION_ENCODE(_MSC_FULL_VER / 10000000, (_MSC_FULL_VER % 10000000) / 100000, (_MSC_FULL_VER % 100000) / 100)

#elif defined(_MSC_FULL_VER) && !defined(__ICL)
#  define COMPILER_MSVC_ENCODED_VERSION COMPILER_VERSION_ENCODE(_MSC_FULL_VER / 1000000, (_MSC_FULL_VER % 1000000) / 10000, (_MSC_FULL_VER % 10000) / 10)

#elif defined(_MSC_VER) && !defined(__ICL)
#  define COMPILER_MSVC_ENCODED_VERSION COMPILER_VERSION_ENCODE(_MSC_VER / 100, _MSC_VER % 100, 0)

#endif

#ifdef COMPILER_MSVC_ENCODED_VERSION
#   define COMPILER_MSVC 1

#   if defined(_MSC_VER) && (_MSC_VER >= 1400)
#       define COMPILER_MSVC_VERSION(major,minor,patch) (_MSC_FULL_VER >= ((major * 10000000) + (minor * 100000) + (patch)))

#   elif defined(_MSC_VER) && (_MSC_VER >= 1200)
#       define COMPILER_MSVC_VERSION(major,minor,patch) (_MSC_FULL_VER >= ((major * 1000000) + (minor * 10000) + (patch)))

#   else
#       define COMPILER_MSVC_VERSION(major,minor,patch) (_MSC_VER >= ((major * 100) + (minor)))

#   endif

#else
#   define COMPILER_MSVC 0
#   define COMPILER_MSVC_VERSION(major, minor, patch) (0)

#endif

#endif /* COMPILER_MSVC_H */