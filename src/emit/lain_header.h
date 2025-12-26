#ifndef EMIT_LAIN_HEADER_H
#define EMIT_LAIN_HEADER_H

#include "../ast.h"    // for Type*, Id
#include "core.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * Rational naming scheme:
 *  - Fixed-length arrays:   Fixed_<Base>_<Len>        e.g. Fixed_u8_5
 *  - Dynamic slices:        Slice_<Base>              e.g. Slice_u8
 *  - Numeric sentinel:      Slice_<Base>_<N>          e.g. Slice_u8_0
 *  - String sentinel:       Slice_<Base>_str<HEX>     e.g. Slice_u8_str9f3a7b2c
 *
 * Header will emit typedefs plus either a _LENGTH or _SENTINEL macro where appropriate.
 */

/* ------------------------- slice registry node --------------------------- */
typedef struct SliceTypeNode {
    char       *sliceName;      // e.g. "Slice_u8", "Fixed_u8_5", "Slice_u8_0", "Slice_u8_str9f3..."
    char       *c_type;         // e.g. "uint8_t", "MyStruct"
    bool        has_len;        // true for dynamic OR fixed-length slices (len field present or length known)
    bool        has_sentinel;   // true if sentinel-terminated (numeric or string)
    bool        sentinel_is_string; // true if sentinel is a string of bytes
    size_t      sentinel_len;   // for string sentinel: length in bytes; for fixed-length: the fixed length
    char       *sentinel_str;   // if string sentinel, pointer to bytes (not necessarily null-terminated)
    int         sentinel_val;   // for numeric sentinel (e.g. 0, 1)
    struct SliceTypeNode *next;
} SliceTypeNode;

// Head of our linked list of slice types
static SliceTypeNode *emitted_slice_types = NULL;

/* ------------------------ small helpers --------------------------------- */

// check by canonical sliceName if already present
static bool slice_type_already_emitted(const char *sliceName) {
    for (SliceTypeNode *n = emitted_slice_types; n; n = n->next) {
        if (strcmp(n->sliceName, sliceName) == 0)
            return true;
    }
    return false;
}

// djb2 hash for sentinel strings -> used to make unique name suffix
static uint32_t fnv1a_hash(const char *data, size_t len) {
    // FNV-1a 32-bit
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= 16777619u;
    }
    return h;
}

// safe strdup+format (small helper)
static char *strdup_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    char *s = malloc((size_t)n + 1);
    memcpy(s, tmp, (size_t)n);
    s[n] = '\0';
    return s;
}

/* ------------------------ record a slice type --------------------------- */
/**
 * Records one slice type for later emission.
 *
 * - sliceName: canonical typedef name to use in C (caller may compute it)
 * - c_type: C element type string (e.g. "uint8_t" or a struct name)
 * - has_len: true for dynamic-length or fixed-length (meaning there is a len or known length)
 * - has_sentinel: true if this is sentinel-terminated (either numeric or string sentinel)
 * - sentinel_is_string: true if sentinel_str contains bytes and sentinel_len > 0
 * - sentinel_str/len: for string sentinel (copied)
 * - sentinel_val: for numeric sentinel (e.g. 0, 1)
 */
static void record_slice_type(const char *sliceName,
                              const char *c_type,
                              bool has_len,
                              bool has_sentinel,
                              bool sentinel_is_string,
                              const char *sentinel_str,
                              size_t sentinel_len,
                              int sentinel_val)
{
    if (slice_type_already_emitted(sliceName))
        return;

    SliceTypeNode *n = malloc(sizeof *n);
    n->sliceName         = strdup(sliceName);
    n->c_type            = strdup(c_type);
    n->has_len           = has_len;
    n->has_sentinel      = has_sentinel;
    n->sentinel_is_string= sentinel_is_string;
    n->sentinel_len      = 0;
    n->sentinel_str      = NULL;
    n->sentinel_val      = 0;

    if (has_sentinel) {
        if (sentinel_is_string && sentinel_str && sentinel_len > 0) {
            n->sentinel_str = malloc(sentinel_len);
            memcpy(n->sentinel_str, sentinel_str, sentinel_len);
            n->sentinel_len = sentinel_len;
        } else {
            // numeric sentinel
            n->sentinel_val = sentinel_val;
        }
    } else {
        // no sentinel; if fixed-length the caller passes sentinel_len>0 to indicate length
        if (has_len && sentinel_len > 0) {
            n->sentinel_len = sentinel_len; // fixed length value
        }
    }

    n->next = emitted_slice_types;
    emitted_slice_types = n;
}

/* ------------------------ emit typedefs into header --------------------- */

static void emit_needed_slice_types(FILE *out) {
    for (SliceTypeNode *n = emitted_slice_types; n; n = n->next) {
        if (n->has_len && n->sentinel_len == 0 && !n->has_sentinel) {
            // 1) dynamic-length slice: has_len==true, no fixed-length value, no sentinel
            fprintf(out, "typedef struct {\n");
            fprintf(out, "  size_t len;\n");
            fprintf(out, "  %s *data;\n", n->c_type);
            fprintf(out, "} %s;\n\n", n->sliceName);
        }
        else if (n->has_sentinel) {
            // 2) sentinel-terminated slice (no length)
            fprintf(out, "typedef struct {\n");
            fprintf(out, "  %s *data;\n", n->c_type);
            fprintf(out, "} %s;\n", n->sliceName);
            // sentinel macro: numeric or string
            if (n->sentinel_is_string && n->sentinel_str && n->sentinel_len > 0) {
                fprintf(out,
                        "#define %s_SENTINEL \"%.*s\"\n\n",
                        n->sliceName,
                        (int)n->sentinel_len,
                        n->sentinel_str);
            } else {
                fprintf(out,
                        "#define %s_SENTINEL %d\n\n",
                        n->sliceName,
                        n->sentinel_val);
            }
        }
        else /* fixed-length: has_len && sentinel_len > 0 */ {
            fprintf(out, "typedef struct {\n");
            fprintf(out, "  %s data[%zu];\n", n->c_type, n->sentinel_len);
            fprintf(out, "} %s;\n", n->sliceName);
            // length macro
            fprintf(out,
                    "#define %s_LENGTH %zu\n\n",
                    n->sliceName,
                    n->sentinel_len);
        }
    }
}

/* ------------------------ header generator ---------------------------- */

static void generate_lain_header(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return;

    // Header guards and includes
    fprintf(f, "#ifndef LAIN_H\n#define LAIN_H\n\n");
    fprintf(f, "#include <stdint.h> /* uint8_t, … */\n");
    fprintf(f, "#include <stddef.h> /* size_t */\n");
    fprintf(f, "#include <stdio.h> /* FILE */\n");
    fprintf(f, "#include <string.h> /* memcmp */\n\n");

    // Emit all recorded slice types
    emit_needed_slice_types(f);

    fprintf(f, "#endif /* LAIN_H */\n");
    fclose(f);
}

/* ------------------------ canonical name generation -------------------- */

/**
 * Create a base token-safe C identifier from an Id (replace '.' with '_').
 * Writes into out (cap chars) and returns out.
 */
static const char *canonical_base_name(Id *base_id, char *out, size_t cap) {
    if (!base_id) {
        strncpy(out, "anon", cap);
        out[cap-1] = '\0';
        return out;
    }
    int L = base_id->length < (int)cap-1 ? (int)base_id->length : (int)cap-1;
    for (int i = 0; i < L; i++) {
        char c = base_id->name[i];
        out[i] = (c == '.') ? '_' : c;
    }
    out[L] = '\0';
    return out;
}

/* ------------------------ main API: get canonical typedef name ------- */

/**
 * Called from your emitter whenever you see an array or slice Type*.
 * Records it for later emission and returns the canonical C typedef name.
 */
const char *emit_slice_type_definition(Type *type) {
    if (!type) return "/*<unknown-slice>*/";

    Type *orig = type;

    // Find element type (base)
    Type *elem = type;
    while (elem && (elem->kind == TYPE_ARRAY || elem->kind == TYPE_SLICE)) {
        elem = elem->element_type;
    }
    if (!elem) return "/*<unknown-slice>*/";

    // base id -> raw name
    Id *base_id = elem->base_type;
    char rawname[64];
    canonical_base_name(base_id, rawname, sizeof rawname);

    // compute C element type
    char c_type[128];
    c_name_for_type(elem, c_type, sizeof c_type);

    static char sliceNameBuf[128];
    // Clear buf
    sliceNameBuf[0] = '\0';

    if (orig->kind == TYPE_ARRAY) {
        // TYPE_ARRAY: could be fixed-length or dynamic (array_len == -1)
        if (orig->array_len >= 0) {
            // fixed-length array
            snprintf(sliceNameBuf, sizeof sliceNameBuf, "Fixed_%s_%ld", rawname, (long)orig->array_len);
            record_slice_type(sliceNameBuf, c_type,
                              /*has_len=*/true,
                              /*has_sentinel=*/false,
                              /*sentinel_is_string=*/false,
                              /*sentinel_str=*/NULL,
                              /*sentinel_len*/ (size_t)orig->array_len,
                              /*sentinel_val=*/0);
            return sliceNameBuf;
        } else {
            // dynamic array (u8[] treated as Slice_<Base>)
            snprintf(sliceNameBuf, sizeof sliceNameBuf, "Slice_%s", rawname);
            record_slice_type(sliceNameBuf, c_type,
                              /*has_len=*/true,  // dynamic -> has len field
                              /*has_sentinel=*/false,
                              /*sentinel_is_string=*/false,
                              /*sentinel_str=*/NULL,
                              /*sentinel_len=*/0,
                              /*sentinel_val=*/0);
            return sliceNameBuf;
        }
    } else if (orig->kind == TYPE_SLICE) {
        // TYPE_SLICE: sentinel-based or fixed-length encoded in sentinel fields
        if (!orig->sentinel_is_string && orig->sentinel_str == NULL && orig->sentinel_len > 0) {
            // (defensive) treat as fixed-length (some code paths used sentinel_len as fixed)
            snprintf(sliceNameBuf, sizeof sliceNameBuf, "Fixed_%s_%ld", rawname, (long)orig->sentinel_len);
            record_slice_type(sliceNameBuf, c_type,
                              /*has_len=*/true,
                              /*has_sentinel=*/false,
                              false,
                              NULL,
                              (size_t)orig->sentinel_len,
                              0);
            return sliceNameBuf;
        }

        if (orig->sentinel_is_string) {
            // string sentinel: produce a hashed suffix to make the name unique
            uint32_t h = fnv1a_hash(orig->sentinel_str ? orig->sentinel_str : "", orig->sentinel_len);
            snprintf(sliceNameBuf, sizeof sliceNameBuf, "Slice_%s_str%08X", rawname, h);
            record_slice_type(sliceNameBuf, c_type,
                              /*has_len=*/false,
                              /*has_sentinel=*/true,
                              /*sentinel_is_string=*/true,
                              orig->sentinel_str,
                              (size_t)orig->sentinel_len,
                              0);
            return sliceNameBuf;
        } else {
            // numeric sentinel, e.g. u8[:0] (orig->sentinel_str contains token text like "0")
            int val = 0;
            if (orig->sentinel_str) {
                val = atoi(orig->sentinel_str);
            }
            snprintf(sliceNameBuf, sizeof sliceNameBuf, "Slice_%s_%d", rawname, val);
            record_slice_type(sliceNameBuf, c_type,
                              /*has_len=*/false,
                              /*has_sentinel=*/true,
                              /*sentinel_is_string=*/false,
                              NULL,
                              0,
                              val);
            return sliceNameBuf;
        }
    } else {
        // fallback: treat as dynamic slice
        snprintf(sliceNameBuf, sizeof sliceNameBuf, "Slice_%s", rawname);
        record_slice_type(sliceNameBuf, c_type,
                          /*has_len=*/true,
                          /*has_sentinel=*/false,
                          false,
                          NULL,
                          0,
                          0);
        return sliceNameBuf;
    }
}

#endif // EMIT_LAIN_HEADER_H
