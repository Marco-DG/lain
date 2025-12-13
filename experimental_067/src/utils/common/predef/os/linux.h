#ifndef OS_LINUX_H
#define OS_LINUX_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__gnu_linux__)
#   define OS_LINUX 1
#endif

#endif /* OS_LINUX_H */