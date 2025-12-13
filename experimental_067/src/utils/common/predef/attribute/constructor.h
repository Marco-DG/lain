#ifndef ATTRIBUTE_CONSTRUCTOR_H
#define ATTRIBUTE_CONSTRUCTOR_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    From:
        https://stackoverflow.com/a/2390626/8411453
*/

#include "../compiler/feature/has_attribute.h"
#include "../compiler//msvc.h"

#if COMPILER_HAS_ATTRIBUTE(constructor)
#   define ATTRIBUTE_CONSTRUCTOR                                            \
        static void __constructor(void) __attribute__((__constructor__));   \
        static void __constructor(void)

/* TODO 
#elif COMPILER_MSVC

#   pragma section(".CRT$XCU", read)
#   define INITIALIZER2_(f, p)                                   \
        static void f(void);                                    \
        __declspec(allocate(".CRT$XCU")) void (*f##_)(void) = f;\
        __pragma(comment(linker,"/include:" p #f "_"))          \
        static void f(void)

#   ifdef _WIN64
    
        #define INITIALIZER(f) INITIALIZER2_(f,"")
#   else
    
        #define INITIALIZER(f) INITIALIZER2_(f,"_")
#   endif
*/

#else
#   warning "Unsupported attribute: constructor"

#endif

#endif /* BUILTIN_CONSTRUCTOR_H */