#ifndef OS_WINDOWS_H
#define OS_WINDOWS_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#if defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) || defined(__TOS_WIN__) || defined(__WINDOWS__)
#   define OS_WINDOWS 1
#endif

#endif /* OS_WINDOWS_H */