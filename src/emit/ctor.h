// emit/ctor.h
#ifndef EMIT_CTOR_H
#define EMIT_CTOR_H

#include <string.h>
#include <stdbool.h>

#define MAX_STRUCT_TYPES 128
static const char* __emitted_structs[MAX_STRUCT_TYPES];
static int         __num_emitted_structs = 0;

/* Subset of registered names that are emitted as scalar typedefs
 * (niche-optimized enums, primitive-aliased typedefs). They behave
 * like primitives at the C ABI level — pass by value, no const T*. */
#define MAX_SCALAR_TYPEDEFS 128
static const char* __emitted_scalar_typedefs[MAX_SCALAR_TYPEDEFS];
static int         __num_emitted_scalar_typedefs = 0;

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

/* Record a scalar-typedef (niche enum / primitive alias). Also marks
 * the name as a struct type so existing callers find it. */
static inline void register_scalar_typedef(const char *cname) {
    register_struct_type(cname);
    if (__num_emitted_scalar_typedefs < MAX_SCALAR_TYPEDEFS)
        __emitted_scalar_typedefs[__num_emitted_scalar_typedefs++] = cname;
}

static inline bool is_scalar_typedef(const char *cname) {
    for (int i = 0; i < __num_emitted_scalar_typedefs; i++) {
        if (strcmp(__emitted_scalar_typedefs[i], cname) == 0)
            return true;
    }
    return false;
}

#endif // EMIT_CTOR_H
