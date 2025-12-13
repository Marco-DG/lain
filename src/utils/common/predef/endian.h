#ifndef ENDIAN_H
#define ENDIAN_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt


    from: https://github.com/boostorg/predef/blob/develop/include/boost/predef/other/endian.h
    see : https://ziglang.org/documentation/master/std/#std.Target.Cpu.Arch.endian

    Detection of endian memory ordering. There are four defined macros
    in this header that define the various generally possible endian
    memory orderings:

    * `ENDIAN_BIG_BYTE`, byte-swapped big-endian.
    * `ENDIAN_BIG_WORD`, word-swapped big-endian.
    * `ENDIAN_LITTLE_BYTE`, byte-swapped little-endian.
    * `ENDIAN_LITTLE_WORD`, word-swapped little-endian.
*/

#if defined(__BYTE_ORDER)
#   if defined(__BIG_ENDIAN) && (__BYTE_ORDER == __BIG_ENDIAN)
#       undef ENDIAN_BIG_BYTE
#       define ENDIAN_BIG_BYTE
#   endif
#   if defined(__LITTLE_ENDIAN) && (__BYTE_ORDER == __LITTLE_ENDIAN)
#       undef ENDIAN_LITTLE_BYTE
#       define ENDIAN_LITTLE_BYTE
#   endif
#   if defined(__PDP_ENDIAN) && (__BYTE_ORDER == __PDP_ENDIAN)
#       undef ENDIAN_LITTLE_WORD
#       define ENDIAN_LITTLE_WORD
#   endif
#endif

#if !defined(__BYTE_ORDER) && defined(_BYTE_ORDER)
#   if defined(_BIG_ENDIAN) && (_BYTE_ORDER == _BIG_ENDIAN)
#       undef ENDIAN_BIG_BYTE
#       define ENDIAN_BIG_BYTE
#   endif
#   if defined(_LITTLE_ENDIAN) && (_BYTE_ORDER == _LITTLE_ENDIAN)
#       undef ENDIAN_LITTLE_BYTE
#       define ENDIAN_LITTLE_BYTE
#   endif
#   if defined(_PDP_ENDIAN) && (_BYTE_ORDER == _PDP_ENDIAN)
#       undef ENDIAN_LITTLE_WORD
#       define ENDIAN_LITTLE_WORD
#   endif
#endif

#endif /* ENDIAN_H */