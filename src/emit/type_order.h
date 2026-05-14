#ifndef EMIT_ORDER_H
#define EMIT_ORDER_H

//=============================================================================
// Order-sensitive emitter: topologically sort enums & structs so that any
// type is fully defined before it’s used in another type’s fields.
//=============================================================================

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "core.h"       // emit_indent(), emit_type(), c_name_for_id()
#include "decl.h"       // emit_decl()
#include "../ast.h"     // Decl, DeclList
#include "../sema.h"    // Type, TYPE_SIMPLE, TYPE_ARRAY, TYPE_SLICE

//-----------------------------------------------------------------------------
// A node in the type‐dependency graph (only enums & structs)
typedef struct {
    Decl   *decl;      // the AST node (DECL_ENUM or DECL_STRUCT)
    char   *name;      // C‐name of the type, e.g. "main_Token"
    int    *deps;      // indices of other TypeNode this one depends on
    int     n_deps;    // number of entries in deps[]
    int     indegree;  // indegree for Kahn’s algorithm
} TypeNode;

// Find the index of a given name in nodes[0..n)
static int find_type_index(TypeNode *nodes, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(nodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

// Include type aliases that resolved to a primitive type — they need
// a `typedef <prim> Name;` emitted before any structure that uses them.
static bool _alias_is_primitive(Decl *d) {
    if (!d || d->kind != DECL_TYPE_ALIAS) return false;
    Expr *e = d->as.type_alias_decl.expr;
    if (!e || e->kind != EXPR_TYPE) return false;
    Type *t = e->as.type_expr.type_value;
    return t && t->kind == TYPE_SIMPLE && t->base_type;
}

// Collect all enums, structs and primitive-aliases into an array of TypeNode.
static void collect_type_nodes(DeclList *decls, TypeNode **out_nodes, int *out_n) {
    int count = 0;
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (dl->decl->kind == DECL_ENUM || dl->decl->kind == DECL_STRUCT)
            count++;
        else if (_alias_is_primitive(dl->decl))
            count++;
    }
    TypeNode *nodes = calloc((size_t)count, sizeof *nodes);

    int idx = 0;
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        const char *cname = NULL;
        if (d->kind == DECL_STRUCT) {
            cname = c_name_for_id(d->as.struct_decl.name);
        } else if (d->kind == DECL_ENUM) {
            cname = c_name_for_id(d->as.enum_decl.type_name);
        } else if (_alias_is_primitive(d)) {
            cname = c_name_for_id(d->as.type_alias_decl.name);
        }
        if (cname) {
            nodes[idx].decl     = d;
            nodes[idx].deps     = NULL;
            nodes[idx].n_deps   = 0;
            nodes[idx].indegree = 0;
            nodes[idx].name = strdup(cname);
            idx++;
        }
    }

    *out_nodes = nodes;
    *out_n     = count;
}

// Helper: register dependency edge (i emits before j when i is in j's
// transitive deps; the existing convention here uses deps[] to store
// "this node's dependents — children that need me first").
static void _add_dep_edge(TypeNode *nodes, int n, Type *ty, int i) {
    while (ty && (ty->kind == TYPE_ARRAY || ty->kind == TYPE_SLICE
                  || ty->kind == TYPE_COMPTIME)) {
        ty = ty->element_type;
    }
    if (ty && ty->kind == TYPE_SIMPLE) {
        const char *ref = c_name_for_id(ty->base_type);
        int j = find_type_index(nodes, n, ref);
        if (j >= 0 && j != i) {
            nodes[j].deps = realloc(
                nodes[j].deps,
                (nodes[j].n_deps + 1) * sizeof *nodes[j].deps
            );
            nodes[j].deps[nodes[j].n_deps++] = i;
        }
    }
    (void)n;  // silence unused-param if find_type_index inlined out
}

// Build dependency edges: struct fields, enum variant fields, and
// alias references all create edges so type-aliases used as enum
// field types are emitted first.
static void build_edges(TypeNode *nodes, int n) {
    for (int i = 0; i < n; i++) {
        Decl *d = nodes[i].decl;
        if (d->kind == DECL_STRUCT) {
            for (DeclList *f = d->as.struct_decl.fields; f; f = f->next) {
                _add_dep_edge(nodes, n, f->decl->as.variable_decl.type, i);
            }
        } else if (d->kind == DECL_ENUM) {
            for (Variant *v = d->as.enum_decl.variants; v; v = v->next) {
                for (DeclList *f = v->fields; f; f = f->next) {
                    _add_dep_edge(nodes, n, f->decl->as.variable_decl.type, i);
                }
            }
        }
        /* DECL_TYPE_ALIAS to a primitive (`int`, `i32`, etc.) has no
           outgoing dep to another tracked node; no-op here. */
    }
    // compute indegrees (child‐counts)
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < nodes[i].n_deps; k++) {
            int child = nodes[i].deps[k];
            nodes[child].indegree++;
        }
    }
}

// Kahn’s algorithm: produce a toposorted array of pointers.
// On cycle, prints an error and exits.
static TypeNode **toposort(TypeNode *nodes, int n, int *out_sorted_n) {
    TypeNode **queue  = malloc((size_t)n * sizeof *queue);
    int        qh = 0, qt = 0;

    // enqueue zero‐indegree nodes
    for (int i = 0; i < n; i++) {
        if (nodes[i].indegree == 0)
            queue[qt++] = &nodes[i];
    }

    TypeNode **sorted = malloc((size_t)n * sizeof *sorted);
    int        cnt    = 0;

    while (qh < qt) {
        TypeNode *u = queue[qh++];
        sorted[cnt++] = u;
        for (int k = 0; k < u->n_deps; k++) {
            TypeNode *v = &nodes[u->deps[k]];
            if (--v->indegree == 0)
                queue[qt++] = v;
        }
    }

    free(queue);

    if (cnt != n) {
        fprintf(stderr, "Error: cyclic dependency among types\n");
        exit(1);
    }

    *out_sorted_n = cnt;
    return sorted;
}

//----------------------------------------------------------------------------
// Public API: emit all enums & structs in dependency order, then all functions.
static void emit_decl_list_topo(DeclList *decls, int depth) {
    //fprintf(stderr, "[DEBUG] in emit_decl_list_topo with %p\n", decls);
    // 1) collect & analyze
    TypeNode *nodes;
    int       n;
    collect_type_nodes(decls, &nodes, &n);

    //debug
    //fprintf(stderr, "[DEBUG] collected %d type-nodes:\n", n);
    //for (int i = 0; i < n; i++) {
    //    fprintf(stderr, "  node %2d: %s\n", i, nodes[i].name);
    //}

    build_edges(nodes, n);
    //debug
    //fprintf(stderr, "[DEBUG] dependencies:\n");
    //for (int i = 0; i < n; i++) {
    //    fprintf(stderr, "  %s depends on:", nodes[i].name);
    //    for (int k = 0; k < nodes[i].n_deps; k++) {
    //        int j = nodes[i].deps[k];
    //        fprintf(stderr, " %s", nodes[j].name);
    //    }
    //    fprintf(stderr, "\n");
    //}

    // 2) toposort
    int        sorted_n;
    TypeNode **sorted = toposort(nodes, n, &sorted_n);
    //debug
    //fprintf(stderr, "[DEBUG] toposorted order (%d nodes):\n", sorted_n);
    //for (int i = 0; i < sorted_n; i++) {
    //    fprintf(stderr, "  %2d: %s\n", i, sorted[i]->name);
    //}

    // 3) emit enums & structs in sorted order
    for (int i = 0; i < sorted_n; i++) {
        emit_decl(sorted[i]->decl, depth);
    }

    // 4) emit all functions in original order
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (dl->decl->kind == DECL_FUNCTION || dl->decl->kind == DECL_PROCEDURE || 
            dl->decl->kind == DECL_EXTERN_FUNCTION || dl->decl->kind == DECL_EXTERN_PROCEDURE ||
            dl->decl->kind == DECL_VARIABLE)
            emit_decl(dl->decl, depth);
    }

    // 5) cleanup
    for (int i = 0; i < n; i++) {
        free(nodes[i].name);
        free(nodes[i].deps);
    }
    free(nodes);
    free(sorted);
}

#endif // EMIT_ORDER_H
