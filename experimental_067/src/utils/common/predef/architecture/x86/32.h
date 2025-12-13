#ifndef ARCHITECTURE_X86_32_H
#define ARCHITECTURE_X86_32_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from:
        https://github.com/cpredef/predef/blob/master/Architectures.md
        https://github.com/simd-everywhere/simde/blob/master/simde/simde-arch.h

    i386, __i386, __i386__, __i486__, __i586__, __i686__    | Defined by GNU C
    __i386		                                            | Defined by Sun Studio
    __i386, __IA32__		                                | Defined by Stratus VOS C
    _M_IX86		                                            | Only defined for 32-bits architectures. Defined by Visual C++, Intel C/C++, Digital Mars, and Watcom C/C++
    __X86__		                                            | Defined by Watcom C/C++
    _X86_		                                            | Defined by MinGW32
    __THW_INTEL__		                                    | Defined by XL C/C++
    __I86__		                                            | Defined by Digital Mars
    __INTEL__		                                        | Defined by CodeWarrior
    __386		                                            | Defined by Diab

    Example:
        CPU               _M_IX86   __I86__
        80386             300       3
        80486             400       4
        Pentium           500       5
        Pentium Pro/II    600       6

*/

#if defined(_M_IX86)
#    define ARCHITECTURE_X86_32 (_M_IX86 / 100)

#elif defined(__I86__)
#    define ARCHITECTURE_X86_32 __I86__

#elif defined(i686) || defined(__i686) || defined(__i686__)
#    define ARCHITECTURE_X86_32 6

#elif defined(i586) || defined(__i586) || defined(__i586__)
#    define ARCHITECTURE_X86_32 5

#elif defined(i486) || defined(__i486) || defined(__i486__)
#    define ARCHITECTURE_X86_32 4

#elif defined(i386) || defined(__i386) || defined(__i386__)
#   define ARCHITECTURE_X86_32 3

#elif defined(_X86_) || defined(__X86__) || defined(__THW_INTEL__)
#   define ARCHITECTURE_X86_32 3

#elif defined(__IA32__) || defined(__INTEL__)
#   define ARCHITECTURE_X86_32 1

#endif

#endif /* ARCHITECTURE_X86_32_H */