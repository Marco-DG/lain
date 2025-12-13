#ifndef ARCHITECTURE_PPC_H
#define ARCHITECTURE_PPC_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from: https://github.com/cpredef/predef/blob/master/Architectures.md
    https://github.com/boostorg/predef/blob/develop/include/boost/predef/architecture/ppc.h

    __powerpc, __powerpc__, __powerpc64__, __POWERPC__, __ppc__,    |
    __ppc64__, __PPC__, __PPC64__, _ARCH_PPC, _ARCH_PPC64           | Defined by GNU C
    _M_PPC	                                                        | Defined by Visual C++
    _ARCH_PPC, _ARCH_PPC64	                                        | Defined by XL C/C++
    __PPCGECKO__	                                                | Gekko Defined by CodeWarrior
    __PPCBROADWAY__	                                                | Broadway Defined by CodeWarrior
    _XENON	                                                        | Xenon
    __ppc	                                                        | Defined by Diab
*/

#if defined(__powerpc64__) || defined(__ppc64__) || defined(__PPC64__) || defined(_ARCH_PPC64)
#   define ARCHITECTURE_PPC 1
#   define ARCHITECTURE_PPC_64 1

#elif defined(__powerpc) || defined(__powerpc__) || defined(__POWERPC__) || defined(__ppc__)  || defined(__PPC__) || defined(_ARCH_PPC) || defined(_M_PPC) || defined(__PPCGECKO__) || defined(__PPCBROADWAY__) || defined(_XENON) || defined(__ppc)
#   define ARCHITECTURE_PPC 1
#   define ARCHITECTURE_PPC_32 1

#endif

#endif /* ARCHITECTURE_PPC_H */