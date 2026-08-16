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
        if (decl_is_generic_template(dl->decl)) continue;   // templates: only instances are emitted
        if (dl->decl->kind == DECL_STRUCT) {
            // Sprint 19: [packed] structs are emitted as scalar typedefs (not
            // C structs), so skip the `typedef struct` forward declaration
            // for them — it would conflict with the scalar typedef.
            if (dl->decl->as.struct_decl.is_packed) continue;
            const char *name = c_name_for_id(dl->decl->as.struct_decl.name);
            EMIT("typedef struct %s %s;\n", name, name);
        } else if (dl->decl->kind == DECL_ENUM) {
            char name[256];
            strncpy(name, c_name_for_id(dl->decl->as.enum_decl.type_name), sizeof name);
            name[sizeof name - 1] = '\0';
            // D-Niche: a niche-optimized enum is a scalar (pointer/int backing),
            // not a struct. Emit its real typedef HERE (complete + early) so the
            // name is usable in the function forward decls that pass it by
            // `const T*`; the definition pass then emits only its constructors.
            // (Same reasoning as the [packed] struct scalar typedef above.)
            NicheLayout __fwd_niche = {0};
            if (enum_is_zero_cost_niche(dl->decl, &__fwd_niche)) {
                char backing[256];
                emit_niche_backing_type(dl->decl, &__fwd_niche, backing, sizeof backing);
                EMIT("typedef %s %s;\n", backing, name);
                register_struct_type(name);
                continue;
            }
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

    // Emit top-level constants BEFORE any function, so function bodies (and later
    // constants like a lookup table) can reference them. `static const` — a
    // module-scoped compile-time constant, folded by the C compiler.
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (!dl->decl || dl->decl->kind != DECL_VARIABLE) continue;
        Decl *d = dl->decl;
        Type *ty = d->as.variable_decl.type;
        // A bare `NAME T = expr` is an immutable compile-time constant (`static
        // const`); a `var NAME T` is a mutable global (`static`, no const).
        const char *cst = d->as.variable_decl.is_mutable ? "static " : "static const ";
        char nm[256]; snprintf(nm, sizeof nm, "%s", c_name_for_id(d->as.variable_decl.name));
        if (ty && ty->kind == TYPE_ARRAY && ty->array_len > 0) {
            char elem[256]; c_name_for_type(ty->element_type, elem, sizeof elem);
            EMIT("%s%s %s[%ld]", cst, elem, nm, (long)ty->array_len);
        } else {
            char tb[256]; c_name_for_type(ty, tb, sizeof tb);
            EMIT("%s%s %s", cst, tb, nm);
        }
        Expr *init = d->as.variable_decl.init;
        if (init) {
            EMIT(" = ");
            if (init->kind == EXPR_ARRAY_LITERAL) {
                EMIT("{ ");
                bool first = true;
                for (ExprList *el = init->as.array_literal_expr.elements; el; el = el->next) {
                    if (!first) EMIT(", ");
                    first = false;
                    emit_expr(el->expr, 0);
                }
                EMIT(" }");
            } else {
                emit_expr(init, 0);
            }
        }
        EMIT(";\n");
    }
    EMIT("\n");

    // Emit the body (function forward decls + definitions) into a temp file so
    // that every slice/array type it references gets recorded before we decide
    // which typedefs to emit. Fall back to writing directly if tmpfile() fails.
    FILE *body = tmpfile();
    output_file = body ? body : real_out;

    // Emit forward declarations for all functions and procedures
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (decl_is_generic_template(dl->decl)) continue;   // templates: only instances are emitted
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
        emit_needed_vector_types(real_out);  // before slices: a slice element may be a vector
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