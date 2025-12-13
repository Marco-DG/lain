#ifndef ARCHITECTURE_SPARC_H
#define ARCHITECTURE_SPARC_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from: https://github.com/cpredef/predef/blob/master/Architectures.md

    __sparc__	                |      Defined by GNU C
    __sparc	                    |      Defined by Sun Studio
    __sparc_v8__, __sparc_v9__	|      Defined by GNU C
    __sparcv8, __sparcv9	    |      Defined by Sun Studio
*/

#if defined(__sparcv9) || defined(__sparc_v9__)
#   define ARCHITECTURE_SPARC 1
#   define ARCHITECTURE_SPARC_V9 1

#elif defined(__sparcv8) || defined(__sparc_v8__)
#   define ARCHITECTURE_SPARC 1
#   define ARCHITECTURE_SPARC_V8 1

#elif defined(__sparc) || defined(__sparc__)
#   define ARCHITECTURE_SPARC 1

#endif

#endif /* ARCHITECTURE_SPARC_H */