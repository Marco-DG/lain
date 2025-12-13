#ifndef ARCHITECTURE_X86_64_H
#define ARCHITECTURE_X86_64_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from: https://github.com/cpredef/predef/blob/master/Architectures.md

    __amd64__, __amd64, __x86_64__, __x86_64    |   Defined by GNU C and Sun Studio
    _M_X64, _M_AMD64                            |   Defined by Visual C++
*/

#if defined(__x86_64__) || defined(__x86_64) || defined(__amd64__) || defined(__amd64) || defined(_M_X64) || defined(_M_AMD64)
#   define ARCHITECTURE_X86_64 1

#endif

#endif /* ARCHITECTURE_X86_64_H */