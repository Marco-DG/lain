#ifndef UTILS_STRNCMP_H
#define UTILS_STRNCMP_H

#include "common.h" /* isize, inline */

//#define str2cmp_macro(ptr, c0, c1) *(ptr+0) == c0 && *(ptr+1) == c1
//#define str3cmp_macro(ptr, c0, c1, c2) *(ptr+0) == c0 && *(ptr+1) == c1 && *(ptr+2) == c2
//#define str4cmp_macro(ptr, c0, c1, c2, c3) *(ptr+0) == c0 && *(ptr+1) == c1 && *(ptr+2) == c2 && *(ptr+3) == c3
//#define str5cmp_macro(ptr, c0, c1, c2, c3, c4) *(ptr+0) == c0 && *(ptr+1) == c1 && *(ptr+2) == c2 && *(ptr+3) == c3 && *(ptr+4) == c4
//#define str6cmp_macro(ptr, c0, c1, c2, c3, c4, c5) *(ptr+0) == c0 && *(ptr+1) == c1 && *(ptr+2) == c2 && *(ptr+3) == c3 && *(ptr+4) == c4 && *(ptr+5) == c5
//
//static inline bool str2cmp(const char* ptr, const char* cmp) {
//    return str2cmp_macro(ptr,  *(cmp+0),  *(cmp+1));
//}
//
//static inline bool str3cmp(const char* ptr, const char* cmp) {
//    return str3cmp_macro(ptr,  *(cmp+0),  *(cmp+1),  *(cmp+2));
//}
//
//static inline bool str4cmp(const char* ptr, const char* cmp) {
//    return str4cmp_macro(ptr,  *(cmp+0),  *(cmp+1),  *(cmp+2), *(cmp+3));
//}
//
//static inline bool str5cmp(const char* ptr, const char* cmp) {
//    return str5cmp_macro(ptr,  *(cmp+0),  *(cmp+1),  *(cmp+2),  *(cmp+3),  *(cmp+4));
//}
//
//static inline bool str6cmp(const char* ptr, const char* cmp) {
//    return str6cmp_macro(ptr,  *(cmp+0),  *(cmp+1),  *(cmp+2),  *(cmp+3),  *(cmp+4), *(cmp+5));
//}

static inline bool strncmp(const char* ptr, const char* cmp, isize n) {
    for (isize i = 0; i < n; i++) {
        if (ptr[i] != cmp[i]) {
            return false;
        }
    }
    return true;
}

#endif /* UTILS_STRNCMP_H */