#ifndef WORDSIZE_H
#define WORDSIZE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "architecture/arm.h"
#include "architecture/ppc.h"
#include "architecture/sparc.h"
#include "architecture/x86.h"

#if ARCHITECTURE_X86_64 || ARCHITECTURE_PPC_64 || ARCHITECTURE_ARM_64 || ARCHITECTURE_SPARC_V9
#   define WORDSIZE_BITS 64

#elif ARCHITECTURE_X86 || ARCHITECTURE_PPC || ARCHITECTURE_ARM || ARCHITECTURE_SPARC
#   define WORDSIZE_BITS 32

#endif

#endif /* WORDSIZE_H */