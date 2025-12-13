#ifndef ARCHITECTURE_X86_16_H
#define ARCHITECTURE_X86_16_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    from: https://github.com/cpredef/predef/blob/master/Architectures.md

    _M_I86		                                            | Only defined for 16-bits architectures. Defined by Visual C++, Digital Mars, and Watcom C/C++ (see note below)
*/

#if defined(_M_I86)
#   define ARCHITECTURE_X86_16 1

#endif

#endif /* ARCHITECTURE_X86_16_H */