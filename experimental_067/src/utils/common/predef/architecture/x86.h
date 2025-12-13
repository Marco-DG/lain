#ifndef ARCHITECTURE_X86_H
#define ARCHITECTURE_X86_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "x86/16.h"
#include "x86/32.h"
#include "x86/64.h"

#if ARCHITECTURE_X86_64 || ARCHITECTURE_X86_32 || ARCHITECTURE_X86_16
#   define ARCHITECTURE_X86 1

#endif

#endif /* ARCHITECTURE_X86_H */