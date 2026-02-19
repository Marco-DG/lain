#ifndef COMPILER_GCC_H
#define COMPILER_GCC_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from:
        https://github.com/nemequ/hedley/tree/master
*/

#include "feature/version.h"
#include "clang.h"
#include "ibm.h"

/* TODO */
/*
#if defined(__GNUC__)  && (                 \
       defined(__clang__)                   \
    || defined(COMPILER_INTEL_VERSION)      \
    || defined(COMPILER_PGI_VERSION)        \
    || defined(COMPILER_ARM_VERSION)        \
    || defined(COMPILER_CRAY_VERSION)       \
    || defined(COMPILER_TI_VERSION)         \
    || defined(COMPILER_TI_ARMCL_VERSION)   \
    || defined(COMPILER_TI_CL430_VERSION)   \
    || defined(COMPILER_TI_CL2000_VERSION)  \
    || defined(COMPILER_TI_CL6X_VERSION)    \
    || defined(COMPILER_TI_CL7X_VERSION)    \
    || defined(COMPILER_TI_CLPRU_VERSION)   \
    || defined(__COMPCERT__)                \
    || defined(COMPILER_MCST_LCC_VERSION)   \
)   
#   define COMPILER_GCC(major, minor, patch) (0)
*/

#if COMPILER_CLANG || COMPILER_IBM /* TODO ... */
    /* do nothing */
#elif defined(__GNUC__) && defined(__GNUC_PATCHLEVEL__)
#   define COMPILER_GCC_ENCODED_VERSION COMPILER_VERSION_ENCODE(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__)

#elif defined(__GNUC__)
#   define COMPILER_GCC_ENCODED_VERSION COMPILER_VERSION_ENCODE(__GNUC__, __GNUC_MINOR__, 0)

#endif

#ifdef COMPILER_GCC_ENCODED_VERSION
#   define COMPILER_GCC 1
#   define COMPILER_GCC_VERSION(major, minor, patch) (COMPILER_GCC_ENCODED_VERSION >= COMPILER_VERSION_ENCODE(major, minor, patch))

#else
#   define COMPILER_GCC 0
#   define COMPILER_GCC_VERSION(major, minor, patch) (0)

#endif

#endif /* COMPILER_GCC_H */