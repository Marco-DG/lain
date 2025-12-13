#ifndef ARCHITECTURE_ARM_H
#define ARCHITECTURE_ARM_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from:
        https://github.com/boostorg/predef/blob/develop/include/boost/predef/architecture/arm.h
        https://github.com/cpredef/predef/blob/master/Architectures.md
        https://wiki.ubuntu.com/ARM/Thumb2PortingHowto

    __arm__	            |   Defined by GNU C and RealView
    __thumb__	        |   Defined by GNU C and RealView in Thumb mode
    __TARGET_ARCH_ARM   |   Defined by RealView
    __TARGET_ARCH_THUMB |   Defined by RealView
    _ARM	            |   Defined by ImageCraft C
    _M_ARM	            |   Defined by Visual Studio
    _M_ARMT	            |   Defined by Visual Studio in Thumb mode
    __arm	            |   Defined by Diab

    ARM 2   |   __ARM_ARCH_2__
    ARM 3   |   __ARM_ARCH_3__, __ARM_ARCH_3M__
    ARM 4   |   __ARM_ARCH_4__
    ARM 4T  |   __ARM_ARCH_4T__, __TARGET_ARM_4T
    ARM 5   |   __ARM_ARCH_5__, __ARM_ARCH_5E__
    ARM 5T  |   __ARM_ARCH_5T__, __ARM_ARCH_5TE__, __ARM_ARCH_5TEJ__
    ARM 6   |   __ARM_ARCH_6__, __ARM_ARCH_6J__, __ARM_ARCH_6K__, __ARM_ARCH_6Z__, __ARM_ARCH_6ZK__
    ARM 6T2 |   __ARM_ARCH_6T2__
    ARM 7   |   __ARM_ARCH_7__, __ARM_ARCH_7A__, __ARM_ARCH_7R__, __ARM_ARCH_7M__, __ARM_ARCH_7S__
    ARM 2   |   __ARM_ARCH_2__
    ARM 3   |   __ARM_ARCH_3__, __ARM_ARCH_3M__
    ARM 4T  |   __ARM_ARCH_4T__, __TARGET_ARM_4T
    ARM 5   |   __ARM_ARCH_5__, __ARM_ARCH_5E__
    ARM 5T  |   __ARM_ARCH_5T__,__ARM_ARCH_5TE__,__ARM_ARCH_5TEJ__
    ARM 6   |   __ARM_ARCH_6__, __ARM_ARCH_6J__, __ARM_ARCH_6K__, __ARM_ARCH_6Z__, __ARM_ARCH_6ZK__
    ARM 6T2 |   __ARM_ARCH_6T2__
    ARM 7   |   __ARM_ARCH_7__, __ARM_ARCH_7A__, __ARM_ARCH_7R__, __ARM_ARCH_7M__, __ARM_ARCH_7S__
*/


#if defined(__ARM_ARCH_2__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_2 1

#elif defined(__ARM_ARCH_3__) || defined(__ARM_ARCH_3M__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_3 1

#elif defined(__ARM_ARCH_4__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_4 1

#elif defined(__ARM_ARCH_4T__) || defined(__TARGET_ARM_4T)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_4T 1

#elif defined(__ARM_ARCH_5__) || defined(__ARM_ARCH_5E__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_5 1

#elif defined(__ARM_ARCH_5T__) || defined(__ARM_ARCH_5TE__) || defined(__ARM_ARCH_5TEJ__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_5T 1

#elif defined(__ARM_ARCH_6__) || defined(__ARM_ARCH_6J__) || defined(__ARM_ARCH_6K__) || defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_6 1

#elif defined(__ARM_ARCH_6T2__) || defined(__ARM_ARCH_6T2__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_6T2 1

#elif defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_7 1

#elif defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_7A 1

#elif defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_7R 1

#elif defined(__ARM_ARCH_7M__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_7M 1

#elif defined(__ARM_ARCH_7S__)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_7S 1

#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__AARCH64EL__) || defined(__arm64)
#   define ARCHITECTURE_ARM 1
#   define ARCHITECTURE_ARM_64 1

#elif defined(__arm__) || defined(__thumb__) || defined(__TARGET_ARCH_ARM) || defined(__TARGET_ARCH_THUMB) || defined(__ARM) || defined(_M_ARM) || defined(_M_ARM_T) || defined(__ARM_ARCH)
#   define ARCHITECTURE_ARM 1

#endif

#endif /* ARCHITECTURE_ARM_H */