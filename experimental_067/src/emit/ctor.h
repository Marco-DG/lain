// emit/ctor.h
#ifndef EMIT_CTOR_H
#define EMIT_CTOR_H

#include <string.h>
#include <stdbool.h>

#define MAX_STRUCT_TYPES 128
static const char* __emitted_structs[MAX_STRUCT_TYPES];
static int         __num_emitted_structs = 0;

/// Record a C‐struct name so we know to append “_ctor” on calls.
static inline void register_struct_type(const char *cname) {
    if (__num_emitted_structs < MAX_STRUCT_TYPES)
        __emitted_structs[__num_emitted_structs++] = cname;
}

/// Return true if this cname was registered as a struct.
static inline bool is_struct_type(const char *cname) {
    for (int i = 0; i < __num_emitted_structs; i++) {
        if (strcmp(__emitted_structs[i], cname) == 0)
            return true;
    }
    return false;
}

#endif // EMIT_CTOR_H
