#ifndef EMIT_H
#define EMIT_H

#include "ast.h"
#include "sema.h"

#include "emit/lain_header.h"
#include "emit/ctor.h"
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
    emit_decl_list_topo(decls, depth);
    fclose(output_file);
    generate_lain_header("../out/lain.h");
}

#endif // EMIT_H