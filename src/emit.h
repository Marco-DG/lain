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

// Entry point: write out C file + generate lain.h
static inline void emit(DeclList *decls, int depth, const char *filename) {
    output_file = fopen(filename, "w");
    if (!output_file) {
        fprintf(stderr, "Error: cannot open %s\n", filename);
        exit(1);
    }
    EMIT("#include \"lain.h\"\n\n");
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
            // D-Niche: niche-optimized enums get a scalar `typedef T E;`
            // here so that later function forward declarations can
            // reference them. The structural `typedef struct E E;` form
            // is skipped (would conflict).
            const char *name = c_name_for_id(dl->decl->as.enum_decl.type_name);
            NicheLayout nl;
            if (enum_is_zero_cost_niche(dl->decl, &nl)) {
                // Choose the same backing type as emit_decl will use.
                Variant *primary = nl.primary_variant
                    ? nl.primary_variant
                    : enum_payload_variant(&dl->decl->as.enum_decl);
                char backing[256];
                if (!primary) {
                    snprintf(backing, sizeof backing, "int32_t");
                } else {
                    Type *ft = primary->fields->decl->as.variable_decl.type;
                    if (nl.pool.kind == POOL_BOOL && !nl.primary_variant) {
                        snprintf(backing, sizeof backing, "uint8_t");
                    } else {
                        c_name_for_type(ft, backing, sizeof backing);
                    }
                }
                EMIT("typedef %s %s;\n", backing, name);
                register_scalar_typedef(name);
            } else {
                EMIT("typedef struct %s %s;\n", name, name);
            }
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

    // Emit forward declarations for all functions and procedures
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (dl->decl->kind == DECL_FUNCTION || dl->decl->kind == DECL_PROCEDURE) {
            emit_forward_decl(dl->decl, 0);
        }
    }
    EMIT("\n");
    emit_decl_list_topo(decls, depth);
    // Emit Fixed_<UserType>_N typedefs that couldn't go in lain.h because
    // they depend on user-defined struct types (complete type required for arrays).
    emit_user_fixed_typedefs(output_file);
    fclose(output_file);
    generate_lain_header("lain.h");
}

#endif // EMIT_H