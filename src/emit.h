#ifndef EMIT_H
#define EMIT_H

#include "ast.h"
#include "sema.h"

#include "emit/ctor.h"
#include "emit/lain_header.h"
#include "emit/core.h"
#include "emit/expr.h"
#include "emit/stmt.h"
#include "emit/decl.h"
#include "emit/type_order.h"

// Entry point: write out C file (all types inlined — no separate lain.h).
//
// Slice/array typedefs (Slice_<T>, Fixed_<T>_N, sentinel variants) are recorded
// lazily as the body is emitted, but must appear BEFORE their first use. Since
// everything now lives in one translation unit, the body is emitted into a temp
// file first (populating the recorded-type list), then the typedefs are written
// ahead of the buffered body. Struct/enum forward typedefs are emitted directly
// so that a Slice_<UserType> (a pointer to a forward-declared struct) is valid.
static inline void emit(DeclList *decls, int depth, const char *filename) {
    FILE *real_out = fopen(filename, "w");
    if (!real_out) {
        fprintf(stderr, "Error: cannot open %s\n", filename);
        exit(1);
    }
    output_file = real_out;
    EMIT("#include <stdint.h>\n");
    EMIT("#include <stddef.h>\n");
    EMIT("#include <stdlib.h>\n");   // abort (panic), malloc/free/realloc/calloc, exit — otherwise implicit-int declarations truncate their pointers on LP64
    EMIT("#include <stdio.h>\n");
    EMIT("#include <string.h>\n");
    EMIT("#include <stdbool.h>\n\n");
    emitted_decls = decls; // so that lookup_function_decl can see all the functions we’re about to emit.

    // Emit forward declarations for structs and enums to satisfy function parameters
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (dl->decl->kind == DECL_STRUCT) {
            // Sprint 19: [packed] structs are emitted as scalar typedefs (not
            // C structs), so skip the `typedef struct` forward declaration
            // for them — it would conflict with the scalar typedef.
            if (dl->decl->as.struct_decl.is_packed) continue;
            const char *name = c_name_for_id(dl->decl->as.struct_decl.name);
            EMIT("typedef struct %s %s;\n", name, name);
        } else if (dl->decl->kind == DECL_ENUM) {
            const char *name = c_name_for_id(dl->decl->as.enum_decl.type_name);
            EMIT("typedef struct %s %s;\n", name, name);
        } else if (dl->decl->kind == DECL_TYPE_ALIAS) {
            // Primitive-aliased typedefs (`type Pressure = i32 >= 0...`)
            // also need to appear before function forward declarations.
            Expr *rhs = dl->decl->as.type_alias_decl.expr;
            if (rhs && rhs->kind == EXPR_TYPE && rhs->as.type_expr.type_value) {
                Type *under = rhs->as.type_expr.type_value;
                if (under->kind == TYPE_SIMPLE && under->base_type) {
                    const char *alias_cn = c_name_for_id(dl->decl->as.type_alias_decl.name);
                    char back[128];
                    c_name_for_type(under, back, sizeof back);
                    EMIT("typedef %s %s;\n", back, alias_cn);
                    register_scalar_typedef(alias_cn);
                }
            }
        }
    }
    EMIT("\n");

    // Emit the body (function forward decls + definitions) into a temp file so
    // that every slice/array type it references gets recorded before we decide
    // which typedefs to emit. Fall back to writing directly if tmpfile() fails.
    FILE *body = tmpfile();
    output_file = body ? body : real_out;

    // Emit forward declarations for all functions and procedures
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (dl->decl->kind == DECL_FUNCTION || dl->decl->kind == DECL_PROCEDURE) {
            emit_forward_decl(dl->decl, 0);
        }
    }
    EMIT("\n");
    emit_decl_list_topo(decls, depth);
    // Emit Fixed_<UserType>_N typedefs that depend on user-defined struct types
    // (complete type required for arrays) — after all struct definitions.
    emit_user_fixed_typedefs(output_file);

    // Now that all slice/array types are recorded, emit the primitive and
    // dynamic-slice typedefs ahead of the body, then splice the body in.
    if (body) {
        emit_needed_slice_types(real_out);
        fflush(body);
        rewind(body);
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, body)) > 0)
            fwrite(buf, 1, n, real_out);
        fclose(body);
    }
    fclose(real_out);
}

#endif // EMIT_H