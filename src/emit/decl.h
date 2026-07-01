#ifndef EMIT_DECL_H
#define EMIT_DECL_H

#include "../emit.h"

// Emit one top-level declaration
void emit_decl(Decl *decl, int depth);
static void emit_forward_decl(Decl *decl, int depth);
// Emit a list of declarations (the whole program)
void emit_decl_list(DeclList *decls, int depth);

static void emit_param_type(Type *t, bool with_restrict); // Forward for use in forward decl

static bool type_contains_typevar(Type *t) {
    while (t) {
        if (t->kind == TYPE_COMPTIME || t->kind == TYPE_VAR) return true;
        if (t->kind == TYPE_ARRAY && t->size_relop == TOKEN_TYPEVAR) return true;
        t = t->element_type;
    }
    return false;
}

static bool is_generic_function(Decl *decl) {
    if (!decl || (decl->kind != DECL_FUNCTION && decl->kind != DECL_PROCEDURE)) return false;
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Type *pt = p->decl->as.variable_decl.type;
        if (pt && type_contains_typevar(pt)) return true;
    }
    return false;
}

// Q-019 [pure]: returns true if any parameter of decl is a var (MODE_MUTABLE) borrow.
// A DECL_FUNCTION with no var params has no caller-visible side effects and qualifies
// for __attribute__((pure)): Lain already guarantees no mutable global access and
// no proc calls, so the only possible side-effect channel is var parameters.
static bool func_has_var_param(Decl *decl) {
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next) {
        if (!p->decl) continue;
        Type *pt = (p->decl->kind == DECL_VARIABLE)   ? p->decl->as.variable_decl.type
                 : (p->decl->kind == DECL_DESTRUCT)    ? p->decl->as.destruct_decl.type
                 : NULL;
        if (pt && pt->mode == MODE_MUTABLE) return true;
    }
    return false;
}

// Q-019c [const]: returns true if every parameter of decl is passed by value in C
// (primitive shared borrows or owned values — no pointer parameters at all).
// When combined with !func_has_var_param, the function qualifies for the stronger
// __attribute__((const)): no pointer params → no indirect memory reads → return
// value depends only on by-value arguments regardless of memory state.
static bool func_all_params_by_value(Decl *decl) {  // also used by func_has_ptr_param
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next) {
        if (!p->decl) continue;
        Type *pt = (p->decl->kind == DECL_VARIABLE) ? p->decl->as.variable_decl.type
                 : (p->decl->kind == DECL_DESTRUCT)  ? p->decl->as.destruct_decl.type
                 : NULL;
        if (!pt) continue;
        if (pt->kind == TYPE_META_TYPE || pt->kind == TYPE_COMPTIME) continue;
        if (pt->kind == TYPE_ARRAY) return false;          // decomposed to ptr
        if (pt->mode == MODE_MUTABLE) return false;        // T * restrict
        if (pt->mode == MODE_SHARED && !is_primitive_type(pt)) return false; // const T*
    }
    return true;
}

// Q-020 [nonnull]: returns true if any parameter becomes a pointer in the C ABI.
// The Lain borrow checker guarantees every borrow (var T or shared T) is a valid,
// non-null reference at the call site — never a null or dangling pointer.
// This mirrors the exact conditions in func_all_params_by_value (inverted).
static bool func_has_ptr_param(Decl *decl) {
    for (DeclList *p = decl->as.function_decl.params; p; p = p->next) {
        if (!p->decl) continue;
        Type *pt = (p->decl->kind == DECL_DESTRUCT)  ? p->decl->as.destruct_decl.type
                 : (p->decl->kind == DECL_VARIABLE)  ? p->decl->as.variable_decl.type
                 : NULL;
        if (!pt) continue;
        if (pt->kind == TYPE_META_TYPE || pt->kind == TYPE_COMPTIME) continue;
        if (pt->kind == TYPE_ARRAY) return true;
        if (pt->mode == MODE_MUTABLE) return true;
        if (pt->mode == MODE_SHARED && !is_primitive_type(pt)) return true;
    }
    return false;
}

// Q-021 [returns_nonnull]: returns true if the function's return type becomes
// a non-null pointer in the C ABI.  Only borrow returns qualify: MODE_MUTABLE
// (var T return → T*) and MODE_SHARED on non-primitives (shared struct → const T*).
// Raw TYPE_POINTER returns are excluded — those may legitimately be null.
static bool func_returns_nonnull_ptr(Decl *decl) {
    if (!decl) return false;
    if (decl->kind != DECL_FUNCTION && decl->kind != DECL_PROCEDURE) return false;
    Type *rt = decl->as.function_decl.return_type;
    if (!rt) return false;
    if (rt->kind == TYPE_META_TYPE || rt->kind == TYPE_COMPTIME) return false;
    if (rt->kind == TYPE_POINTER) return false;  // raw pointer: may be null
    if (rt->mode == MODE_MUTABLE) return true;
    // MODE_SHARED non-primitive: only pointer types return as C pointers (not structs)
    if (rt->mode == MODE_SHARED && rt->kind == TYPE_POINTER) return true;
    if (rt->mode == MODE_SHARED && rt->kind == TYPE_ARRAY && rt->array_len == -1) return true;
    return false;
}

static void emit_forward_decl(Decl *decl, int depth) {
    if (!decl) return;
    if (is_generic_function(decl)) return;
    if (decl->kind == DECL_FUNCTION || decl->kind == DECL_PROCEDURE) {
        if (decl->as.function_decl.return_type && decl->as.function_decl.return_type->kind == TYPE_META_TYPE) return; // Pure CTFE function
        
        emit_indent(depth);
        // Q-019 [pure/const]: forward-declare func without var params as
        // __attribute__((const)) when all params are by-value, otherwise
        // __attribute__((pure)). Both enable LICM/CSE; const is stronger.
        if (decl->kind == DECL_FUNCTION && !func_has_var_param(decl)) {
            if (func_all_params_by_value(decl))
                EMIT("__attribute__((const)) ");
            else
                EMIT("__attribute__((pure)) ");
        }
        // Q-020 [nonnull]: every borrow (var T or shared T) is provably non-null;
        // emit nonnull with no args to cover all pointer parameters at once.
        if (func_has_ptr_param(decl))
            EMIT("__attribute__((nonnull)) ");
        // Q-021 [returns_nonnull]: borrow return types are provably non-null pointers.
        if (func_returns_nonnull_ptr(decl))
            EMIT("__attribute__((returns_nonnull)) ");
        if (decl->as.function_decl.return_type) {
            emit_type(decl->as.function_decl.return_type);
        } else {
            const char *_fn = decl->as.function_decl.name->name;
            size_t _fl = decl->as.function_decl.name->length;
            if (_fl == 4 && strncmp(_fn, "main", 4) == 0)
                EMIT("int32_t"); // C99: main must return int
            else
                EMIT("void");
        }

        const char *id_name = decl->as.function_decl.name->name;
        size_t id_len = decl->as.function_decl.name->length;
        if (id_len == 4 && strncmp(id_name, "main", 4) == 0) {
            EMIT(" main(");
        } else {
            EMIT(" %s(", c_name_for_id(decl->as.function_decl.name));
        }

        DeclList* param = decl->as.function_decl.params;
        if (param) {
            int first = 1;
            while (param) {
                if (param->decl->kind == DECL_VARIABLE && param->decl->as.variable_decl.type && (param->decl->as.variable_decl.type->kind == TYPE_META_TYPE || param->decl->as.variable_decl.type->kind == TYPE_COMPTIME)) {
                    param = param->next;
                    continue;
                }
                if (!first) {
                    EMIT(", ");
                }
                if (param->decl->kind == DECL_DESTRUCT) {
                    emit_param_type(param->decl->as.destruct_decl.type, true);
                } else {
                    Type *pt = param->decl->as.variable_decl.type;
                    Id   *pn = param->decl->as.variable_decl.name;
                    if (pt && pt->kind == TYPE_ARRAY && pt->array_len == -1) {
                        // Fase 7: decompose dynamic array param to size_t? + T *
                        char elem_buf[256];
                        c_name_for_type(pt->element_type, elem_buf, sizeof elem_buf);
                        if (pt->size_expr == NULL)
                            EMIT("size_t __len_%.*s, ", (int)pn->length, pn->name);
                        if (pt->mode == MODE_MUTABLE)
                            EMIT("%s * restrict", elem_buf);
                        else
                            EMIT("const %s*", elem_buf);
                    } else {
                        emit_param_type(pt, true);
                    }
                }
                first = 0;
                param = param->next;
            }
        } else {
            EMIT("void");
        }
        EMIT(");\n");
    }
}

static void emit_param_type(Type *t, bool with_restrict) {
    if (!t) return;

    // Get the base type name without ownership decorations,
    // BUT for Pointers, the mode dictates the C type (const vs non-const),
    // so we must preserve it to generate the correct base "value type".
    OwnershipMode original_mode = t->mode;
    if (t->kind != TYPE_POINTER) {
        t->mode = MODE_SHARED;
    }

    char base_name[256];
    c_name_for_type(t, base_name, sizeof(base_name));

    t->mode = original_mode;  // restore original mode

    // Now emit the correct C type based on ownership mode
    if (original_mode == MODE_OWNED) {
        // mov T -> pass by value (T)
        EMIT("%s", base_name);
    } else if (original_mode == MODE_MUTABLE) {
        // mut T -> pass as mutable pointer.
        // with_restrict=true for Lain functions: the borrow checker has
        // already proven at every call site that no two mut parameters alias,
        // so `restrict` is a sound annotation and enables SIMD vectorization.
        if (with_restrict) {
            EMIT("%s * restrict", base_name);
        } else {
            EMIT("%s *", base_name);
        }
    } else {
        // Shared Reference (MODE_SHARED)
        if (is_primitive_type(t)) {
            EMIT("%s", base_name);  // Pass by value for primitives
        } else {
            EMIT("const %s*", base_name);  // Pass as const pointer for structs
        }
    }
}

void emit_decl(Decl* decl, int depth) {
    if (!decl) return;
    switch (decl->kind) {
        case DECL_VARIABLE:
            // Emit a top-level variable declaration: <type> <name>;
            emit_indent(depth);
            emit_type(decl->as.variable_decl.type);
            EMIT(" %s;\n",
                c_name_for_id(decl->as.variable_decl.name));
            break;

        case DECL_EXTERN_PROCEDURE:
        case DECL_EXTERN_FUNCTION: {
             emit_indent(depth);
             EMIT("extern ");
             const char *fname = c_name_for_id(decl->as.function_decl.name);
             if (strcmp(fname, "fgets") == 0) {
                 EMIT("char *");
             } else if (decl->as.function_decl.return_type) {
                 emit_type(decl->as.function_decl.return_type);
             } else {
                 EMIT("void");
             }
             EMIT(" %s(", fname);
            
             DeclList* param = decl->as.function_decl.params;
             if (param) {
                 int first = 1;
                 while (param) {
                     if (!first) EMIT(", ");
                     if (param->decl->kind == DECL_DESTRUCT) {
                          emit_param_type(param->decl->as.destruct_decl.type, false);
                          EMIT(" _destruct_param_");
                     } else {
                          Type *pt = param->decl->as.variable_decl.type;
                          const char *fname = c_name_for_id(decl->as.function_decl.name);
                          // Hack: force const char* for puts/printf
                          if (pt->kind == TYPE_POINTER && pt->element_type->kind == TYPE_SIMPLE &&
                              (
                                // Strings: *char, *u8
                                ((pt->element_type->base_type->length == 4 && strncmp(pt->element_type->base_type->name, "char", 4) == 0) ||
                                 (pt->element_type->base_type->length == 2 && strncmp(pt->element_type->base_type->name, "u8", 2) == 0))
                                ||
                                // FILE handles: *FILE (Shared) -> FILE *
                                (pt->element_type->base_type->length == 4 && strncmp(pt->element_type->base_type->name, "FILE", 4) == 0)
                              ) &&
                              (strcmp(fname, "puts") == 0 || strcmp(fname, "printf") == 0 ||
                               strcmp(fname, "libc_puts") == 0 || strcmp(fname, "libc_printf") == 0 ||
                               strcmp(fname, "fopen") == 0 || strcmp(fname, "fputs") == 0 ||
                               strcmp(fname, "fgets") == 0))
                          {
                              // C-Interop: Map u8* to char* and FILE* to FILE* (mut)
                              Id *base = pt->element_type->base_type;
                              if (base->length == 4 && strncmp(base->name, "FILE", 4) == 0) {
                                  EMIT("FILE *"); // Always mutable FILE* for libc
                              } else {
                                  // Strings (u8*)
                                  if (pt->mode == MODE_MUTABLE || pt->mode == MODE_OWNED) {
                                      EMIT("char *");
                                  } else {
                                      EMIT("const char *");
                                  }
                              }
                          } else {
                              emit_param_type(pt, false);
                          }
                          EMIT(" %.*s",
                               (int)param->decl->as.variable_decl.name->length,
                               param->decl->as.variable_decl.name->name);
                     }
                     first = 0;
                     param = param->next;
                 }
                  if (decl->as.function_decl.is_variadic) {
                      if (!first) EMIT(", ");
                      EMIT("...");
                  }
              } else {
                  if (decl->as.function_decl.is_variadic) {
                      EMIT("...");
                 } else {
                     EMIT("void");
                 }
             }
             EMIT(");\n");
             break;
        }

        case DECL_PROCEDURE:
        case DECL_FUNCTION: {
            if (is_generic_function(decl)) return;
            if (decl->as.function_decl.return_type && decl->as.function_decl.return_type->kind == TYPE_META_TYPE) return; // Pure CTFE function

            // Q-017 [fast_math]: emit pragma to enable FMA fusion for this function
            bool has_fast_math = false;
            for (Attr *a = decl->attributes; a; a = a->next) {
                if (a->name && a->name->length == 9 && strncmp(a->name->name, "fast_math", 9) == 0) {
                    has_fast_math = true;
                    break;
                }
            }
            if (has_fast_math) {
                emit_indent(depth);
                EMIT("#pragma GCC push_options\n");
                emit_indent(depth);
                EMIT("#pragma GCC optimize(\"fp-contract=fast\")\n");
            }

            emit_indent(depth);
            // Q-018 [private]: emit `static` for private declarations (module-internal linkage)
            // skip for main (must be extern)
            const char *fname = decl->as.function_decl.name->name;
            size_t flen = decl->as.function_decl.name->length;
            bool is_main = (flen == 4 && strncmp(fname, "main", 4) == 0);
            if (decl->is_private && !is_main) {
                EMIT("static ");
            }
            // Q-019 [pure/const]: func without var params gets __attribute__((const))
            // when all params are by value (no pointer args → no indirect reads),
            // or __attribute__((pure)) otherwise. Both allow LICM/CSE; const is
            // the stronger guarantee and allows hoisting even when memory changes.
            if (decl->kind == DECL_FUNCTION && !is_main && !func_has_var_param(decl)) {
                if (func_all_params_by_value(decl))
                    EMIT("__attribute__((const)) ");
                else
                    EMIT("__attribute__((pure)) ");
            }
            // Q-020 [nonnull]: borrow checker proves every pointer param is non-null.
            if (!is_main && func_has_ptr_param(decl))
                EMIT("__attribute__((nonnull)) ");
            // Q-021 [returns_nonnull]: borrow return type is provably non-null.
            if (!is_main && func_returns_nonnull_ptr(decl))
                EMIT("__attribute__((returns_nonnull)) ");
            // Print return type and function name.
            if (decl->as.function_decl.return_type) {
                emit_type(decl->as.function_decl.return_type);
            } else if (is_main) {
                EMIT("int32_t"); // C99: main must return int
            } else {
                EMIT("void");
            }

            // special case if the function is named "main"
            const char *id_name = decl->as.function_decl.name->name;
            size_t id_len = decl->as.function_decl.name->length;
            if (id_len == 4 && strncmp(id_name, "main", 4) == 0) {
                EMIT(" main(");
            } else {
                EMIT(" %s(", c_name_for_id(decl->as.function_decl.name));
            }

            DeclList* param = decl->as.function_decl.params;
            if (param) {
                int first = 1;
                int param_idx = 0;
                while (param) {
                    if (param->decl->kind == DECL_VARIABLE && param->decl->as.variable_decl.type && (param->decl->as.variable_decl.type->kind == TYPE_META_TYPE || param->decl->as.variable_decl.type->kind == TYPE_COMPTIME)) {
                        param = param->next;
                        param_idx++;
                        continue;
                    }
                    if (!first) {
                        EMIT(", ");
                    }
                    
                    if (param->decl->kind == DECL_DESTRUCT) {
                        // Emit: Type _param_N
                        emit_param_type(param->decl->as.destruct_decl.type, true);
                        EMIT(" _param_%d", param_idx);
                    } else {
                        Type *pt = param->decl->as.variable_decl.type;
                        Id   *pn = param->decl->as.variable_decl.name;
                        if (pt && pt->kind == TYPE_ARRAY && pt->array_len == -1) {
                            // Fase 7: decompose dynamic array param to (size_t __len_X,)? T * X
                            char elem_buf[256];
                            c_name_for_type(pt->element_type, elem_buf, sizeof elem_buf);
                            if (pt->size_expr == NULL)
                                EMIT("size_t __len_%.*s, ", (int)pn->length, pn->name);
                            if (pt->mode == MODE_MUTABLE)
                                EMIT("%s * restrict %.*s", elem_buf, (int)pn->length, pn->name);
                            else
                                EMIT("const %s* %.*s", elem_buf, (int)pn->length, pn->name);
                        } else {
                            // Use emit_param_type to print parameter type.
                            emit_param_type(pt, true);
                            EMIT(" %.*s", (int)pn->length, pn->name);
                        }
                    }
                    first = 0;
                    param = param->next;
                    param_idx++;
                }
            } else {
                EMIT("void");
            }
            EMIT(") {\n");

            // Inject destructuring initialization
            param = decl->as.function_decl.params;
            int param_idx = 0;
            while (param) {
                if (param->decl->kind == DECL_DESTRUCT) {
                    DeclDestruct *dd = &param->decl->as.destruct_decl;
                    
                    // Resolve struct to find field types
                    Decl *struct_decl = NULL;
                    if (dd->type->kind == TYPE_SIMPLE) {
                        for (DeclList *g = emitted_decls; g; g = g->next) {
                            if (g->decl->kind == DECL_STRUCT) {
                                Id *sname = g->decl->as.struct_decl.name;
                                if (sname->length == dd->type->base_type->length &&
                                    strncmp(sname->name, dd->type->base_type->name, sname->length) == 0) {
                                    struct_decl = g->decl;
                                    break;
                                }
                            }
                        }
                    }

                    if (struct_decl) {
                        for (IdList *n = dd->names; n; n = n->next) {
                            // Find field type
                            Type *field_type = NULL;
                            for (DeclList *f = struct_decl->as.struct_decl.fields; f; f = f->next) {
                                Id *fname = f->decl->as.variable_decl.name;
                                if (fname->length == n->id->length &&
                                    strncmp(fname->name, n->id->name, fname->length) == 0) {
                                    field_type = f->decl->as.variable_decl.type;
                                    break;
                                }
                            }
                            
                            if (field_type) {
                                emit_indent(depth + 1);
                                emit_type(field_type);
                                
                                // Check if passed by value (OWNED) or pointer (SHARED/MUTABLE)
                                const char *op = "->";
                                if (dd->type->mode == MODE_OWNED) {
                                    op = ".";
                                }

                                EMIT(" %.*s = _param_%d%s%.*s;\n",
                                     (int)n->id->length, n->id->name,
                                     param_idx,
                                     op,
                                     (int)n->id->length, n->id->name);
                            }
                        }
                    }
                }
                param = param->next;
                param_idx++;
            }

            emit_stmt_list(decl->as.function_decl.body, depth + 1);
            emit_indent(depth);
            EMIT("}\n");

            // Q-017 [fast_math]: pop pragma options after function body
            if (has_fast_math) {
                emit_indent(depth);
                EMIT("#pragma GCC pop_options\n");
            }
            EMIT("\n");
            break;
        }


        case DECL_STRUCT: {
            const char *structName = c_name_for_id(decl->as.struct_decl.name);

            // Sprint 19: [packed] struct — bit-exact layout.
            // Each iN/uN field occupies exactly N bits within a scalar
            // container. We emit a typedef to the smallest stdint type
            // that fits the total bit-width and inline getter functions
            // that extract each field via shift+mask.
            if (decl->as.struct_decl.is_packed) {
                // Pass 1: compute total bit-width and per-field offsets.
                int total_bits = 0;
                struct { int bits; int offset; bool ok; } pf[64];
                int n_fields = 0;
                bool fields_ok = true;
                for (DeclList *f = decl->as.struct_decl.fields; f && n_fields < 64; f = f->next) {
                    if (!f->decl || f->decl->kind != DECL_VARIABLE) {
                        fields_ok = false; break;
                    }
                    Type *ft = f->decl->as.variable_decl.type;
                    if (!ft || ft->kind != TYPE_SIMPLE || !ft->base_type) {
                        fields_ok = false; break;
                    }
                    const char *fname = ft->base_type->name;
                    isize flen = ft->base_type->length;
                    if (flen < 2 || flen > 3 || (fname[0] != 'i' && fname[0] != 'u')) {
                        fields_ok = false; break;
                    }
                    int n = 0; bool ok_digits = true;
                    for (isize k = 1; k < flen; k++) {
                        if (fname[k] < '0' || fname[k] > '9') { ok_digits = false; break; }
                        n = n * 10 + (fname[k] - '0');
                    }
                    if (!ok_digits || n < 1 || n > 64) {
                        fields_ok = false; break;
                    }
                    pf[n_fields].bits = n;
                    pf[n_fields].offset = total_bits;
                    pf[n_fields].ok = true;
                    total_bits += n;
                    n_fields++;
                }
                if (!fields_ok || total_bits == 0 || total_bits > 64) {
                    fprintf(stderr,
                        "[E121] [packed] struct '%s' invalid: only iN/uN fields with total ≤ 64 bits supported (got %d bits).\n",
                        structName, total_bits);
                    exit(1);
                }
                const char *container =
                    (total_bits <= 8)  ? "uint8_t"  :
                    (total_bits <= 16) ? "uint16_t" :
                    (total_bits <= 32) ? "uint32_t" :
                                         "uint64_t";
                int container_bits =
                    (total_bits <= 8)  ?  8 :
                    (total_bits <= 16) ? 16 :
                    (total_bits <= 32) ? 32 : 64;
                (void)container_bits;

                emit_indent(depth);
                EMIT("typedef %s %s;\n", container, structName);
                register_struct_type(structName);

                // Constructor: takes all fields, returns packed container.
                emit_indent(depth);
                EMIT("static inline %s %s_ctor(", structName, structName);
                int idx = 0;
                int first = 1;
                for (DeclList *f = decl->as.struct_decl.fields; f; f = f->next, idx++) {
                    if (!first) EMIT(", ");
                    emit_type(f->decl->as.variable_decl.type);
                    EMIT(" %.*s",
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name);
                    first = 0;
                }
                EMIT(") {\n");
                emit_indent(depth + 1);
                EMIT("return (%s)(", container);
                idx = 0; first = 1;
                for (DeclList *f = decl->as.struct_decl.fields; f; f = f->next, idx++) {
                    if (!first) EMIT(" | ");
                    int bits = pf[idx].bits;
                    int off  = pf[idx].offset;
                    unsigned long long mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
                    EMIT("(((%s)(%.*s) & 0x%llxULL) << %d)",
                         container,
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name,
                         mask, off);
                    first = 0;
                }
                EMIT(");\n");
                emit_indent(depth);
                EMIT("}\n\n");

                // Per-field getter functions.
                // (Setters were removed for philosophical coherence with P5
                //  "no hidden magic": `r.pin1 = 9` looks like assignment but
                //  was rewritten as an inline function call. To modify a
                //  packed struct field, reconstruct the struct explicitly:
                //  `r = GpioModer(r.pin0, 9, r.pin2, r.pin3)`. This makes
                //  the cost and semantics visible at the call site.)
                idx = 0;
                for (DeclList *f = decl->as.struct_decl.fields; f; f = f->next, idx++) {
                    int bits = pf[idx].bits;
                    int off  = pf[idx].offset;
                    unsigned long long mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);

                    // Getter: extract bit range.
                    emit_indent(depth);
                    EMIT("static inline ");
                    emit_type(f->decl->as.variable_decl.type);
                    EMIT(" %s_get_%.*s(%s r) {\n",
                         structName,
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name,
                         structName);
                    emit_indent(depth + 1);
                    EMIT("return (");
                    emit_type(f->decl->as.variable_decl.type);
                    EMIT(")((r >> %d) & 0x%llxULL);\n", off, mask);
                    emit_indent(depth);
                    EMIT("}\n\n");
                }
                break;
            }

            // 1) struct definition
            emit_indent(depth);
            EMIT("typedef struct %s {\n", structName);

            // 2) fields
            for (DeclList* field = decl->as.struct_decl.fields; field; field = field->next) {
                if (field->decl) {
                    emit_indent(depth + 1);
                    emit_type(field->decl->as.variable_decl.type);
                    EMIT(" %.*s;\n",
                         (int)field->decl->as.variable_decl.name->length,
                         field->decl->as.variable_decl.name->name);
                } else {
                    emit_indent(depth + 1);
                    EMIT("/* NULL field in struct %s */\n", structName);
                }
            }

            // 3) close typedef
            emit_indent(depth);
            EMIT("} %s;\n\n", structName);
            register_struct_type(structName);

            // 4) inline “constructor” function
            //    static inline StructName StructName_ctor(field1_type f1, field2_type f2, …) { … }
            emit_indent(depth);
            EMIT("static inline %s %s_ctor(", structName, structName);
            // parameters
            {
                int first = 1;
                for (DeclList* f = decl->as.struct_decl.fields; f; f = f->next) {
                    if (!f->decl) continue;
                    if (!first) EMIT(", ");
                    emit_type(f->decl->as.variable_decl.type);
                    EMIT(" %.*s",
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name);
                    first = 0;
                }
            }
            EMIT(") {\n");

            // body: return (StructName){ .field1 = field1, .field2 = field2, … };
            emit_indent(depth + 1);
            EMIT("return (%s){ ", structName);
            {
                int first = 1;
                for (DeclList* f = decl->as.struct_decl.fields; f; f = f->next) {
                    if (!f->decl) continue;
                    if (!first) EMIT(", ");
                    EMIT(".%.*s = %.*s",
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name,
                         (int)f->decl->as.variable_decl.name->length,
                         f->decl->as.variable_decl.name->name);
                    first = 0;
                }
            }
            EMIT(" };\n");

            // 5) close constructor function
            emit_indent(depth);
            EMIT("}\n\n");

            break;
        }



        case DECL_ENUM: {
            // 1) lookup the C‐enum tag, e.g. "main_Shape"
            const char *adt_name = c_name_for_id(decl->as.enum_decl.type_name);

            // D-Niche M3: branch on layout. Zero-cost layouts use a
            // direct typedef of the payload/sentinel type; legacy
            // tag+union is the fallback path.
            NicheLayout __niche_layout = {0};
            bool __use_niche = enum_is_zero_cost_niche(decl, &__niche_layout);

            if (__use_niche) {
                bool is_multi = (__niche_layout.primary_variant != NULL &&
                                 __niche_layout.secondary_variant != NULL);

                // Pick the C backing type.
                //  - Multi-payload niche:    primary's field type
                //  - Pure-empty enum:        int32_t
                //  - Single payload pointer: that pointer type
                //  - Single payload bool:    uint8_t
                //  - Other:                  payload field's type
                char backing[256];
                Variant *primary = is_multi
                    ? __niche_layout.primary_variant
                    : enum_payload_variant(&decl->as.enum_decl);
                if (!primary) {
                    snprintf(backing, sizeof backing, "int32_t");
                } else {
                    Type *ft = primary->fields->decl->as.variable_decl.type;
                    if (!is_multi && __niche_layout.pool.kind == POOL_BOOL) {
                        snprintf(backing, sizeof backing, "uint8_t");
                    } else {
                        c_name_for_type(ft, backing, sizeof backing);
                    }
                }

                if (!is_struct_type(adt_name)) {
                    emit_indent(depth);
                    EMIT("typedef %s %s;\n\n", backing, adt_name);
                    register_scalar_typedef(adt_name);
                }

                // Constructors. Each variant gets a static inline function;
                // empty variants return the assigned sentinel, payload
                // variants are identity functions on the payload field.
                // Multi-payload secondary: maps sub-enum value -> sentinel.
                for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                    emit_indent(depth);
                    EMIT("static inline %s %s_%.*s(", adt_name, adt_name,
                         (int)v->name->length, v->name->name);
                    if (v->fields) {
                        int first = 1;
                        for (DeclList *f = v->fields; f; f = f->next) {
                            if (!first) EMIT(", ");
                            emit_type(f->decl->as.variable_decl.type);
                            EMIT(" %.*s",
                                 (int)f->decl->as.variable_decl.name->length,
                                 f->decl->as.variable_decl.name->name);
                            first = 0;
                        }
                    } else {
                        EMIT("void");
                    }
                    EMIT(") {\n");
                    emit_indent(depth + 1);

                    if (is_multi && v == __niche_layout.secondary_variant) {
                        // Secondary: map sub-enum value to sentinel via
                        // (value * stride). Sub-enum's empties are 0,1,2,...
                        // and primary pool sentinels are 0, stride, 2*stride, ...
                        Id *fname = v->fields->decl->as.variable_decl.name;
                        long long stride = __niche_layout.pool.ptr_stride > 0
                            ? __niche_layout.pool.ptr_stride : 1;
                        if (__niche_layout.pool.kind == POOL_POINTER) {
                            EMIT("return (%s)(uintptr_t)((long long)%.*s * %lldLL);\n",
                                 backing,
                                 (int)fname->length, fname->name, stride);
                        } else {
                            EMIT("return (%s)((long long)%.*s * %lldLL);\n",
                                 backing,
                                 (int)fname->length, fname->name, stride);
                        }
                    } else if (!v->fields) {
                        long long s = niche_sentinel_for_variant(
                            &decl->as.enum_decl, v, &__niche_layout);
                        if (__niche_layout.pool.kind == POOL_POINTER) {
                            EMIT("return (%s)(uintptr_t)%lldLL;\n",
                                 backing, s);
                        } else {
                            EMIT("return (%s)%lldLL;\n", backing, s);
                        }
                    } else {
                        // Identity on the single field (primary in multi-payload
                        // OR sole payload in single-payload niche).
                        Id *fname = v->fields->decl->as.variable_decl.name;
                        if (__niche_layout.pool.kind == POOL_BOOL && !is_multi) {
                            EMIT("return (uint8_t)%.*s;\n",
                                 (int)fname->length, fname->name);
                        } else {
                            EMIT("return %.*s;\n",
                                 (int)fname->length, fname->name);
                        }
                    }
                    emit_indent(depth);
                    EMIT("}\n\n");
                }
                break;  // skip legacy tag+union emit
            }

            // 2) Generate the Tag Enum: typedef enum { Shape_Tag_Circle, Shape_Tag_Rectangle } Shape_Tag;
            emit_indent(depth);
            EMIT("typedef enum {\n");

            for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                emit_indent(depth + 1);
                EMIT("%s_Tag_%.*s,\n", adt_name, (int)v->name->length, v->name->name);
            }

            emit_indent(depth);
            EMIT("} %s_Tag;\n\n", adt_name);

            // 3) Generate the ADT Struct: typedef struct Shape { Shape_Tag tag; union { ... } data; } Shape;
            emit_indent(depth);
            EMIT("typedef struct %s {\n", adt_name);
            emit_indent(depth + 1);
            EMIT("%s_Tag tag;\n", adt_name);
            
            // Only generate union if there are variants with fields
            bool has_fields = false;
            for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                if (v->fields) {
                    has_fields = true;
                    break;
                }
            }
            
            if (has_fields) {
                emit_indent(depth + 1);
                EMIT("union {\n");
                
                for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                    if (v->fields) {
                        emit_indent(depth + 2);
                        EMIT("struct {\n");
                        for (DeclList *f = v->fields; f; f = f->next) {
                            emit_indent(depth + 3);
                            emit_type(f->decl->as.variable_decl.type);
                            EMIT(" %.*s;\n", 
                                 (int)f->decl->as.variable_decl.name->length,
                                 f->decl->as.variable_decl.name->name);
                        }
                        emit_indent(depth + 2);
                        EMIT("} %.*s;\n", (int)v->name->length, v->name->name);
                    }
                }
                
                emit_indent(depth + 1);
                EMIT("} data;\n");
            }
            
            emit_indent(depth);
            EMIT("} %s;\n\n", adt_name);
            
            register_struct_type(adt_name); // Register as a type so it can be used

            // 4) Generate Constructors
            // static inline Shape Shape_Circle(int radius) { return (Shape){ .tag = Shape_Tag_Circle, .data.Circle = { radius } }; }
            for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                emit_indent(depth);
                EMIT("static inline %s %s_%.*s(", adt_name, adt_name, (int)v->name->length, v->name->name);
                
                // Params
                if (v->fields) {
                    int first = 1;
                    for (DeclList *f = v->fields; f; f = f->next) {
                        if (!first) EMIT(", ");
                        emit_type(f->decl->as.variable_decl.type);
                        EMIT(" %.*s", 
                             (int)f->decl->as.variable_decl.name->length,
                             f->decl->as.variable_decl.name->name);
                        first = 0;
                    }
                }
                
                EMIT(") {\n");
                emit_indent(depth + 1);
                EMIT("return (%s){ .tag = %s_Tag_%.*s", adt_name, adt_name, (int)v->name->length, v->name->name);
                
                if (v->fields) {
                    EMIT(", .data.%.*s = { ", (int)v->name->length, v->name->name);
                    int first = 1;
                    for (DeclList *f = v->fields; f; f = f->next) {
                        if (!first) EMIT(", ");
                        EMIT(".%.*s = %.*s", 
                             (int)f->decl->as.variable_decl.name->length, f->decl->as.variable_decl.name->name,
                             (int)f->decl->as.variable_decl.name->length, f->decl->as.variable_decl.name->name);
                        first = 0;
                    }
                    EMIT(" }");
                }
                
                EMIT(" };\n");
                emit_indent(depth);
                EMIT("}\n\n");
            }
        }
        break;


        case DECL_C_INCLUDE: {
            const char* path = decl->as.c_include_decl.path;
            emit_indent(depth);
            if (path[0] == '<') {
                EMIT("#include %s\n", path);
            } else {
                EMIT("#include \"%s\"\n", path);
            }
            break;
        }

        case DECL_EXTERN_TYPE: {
            const char* name = c_name_for_id(decl->as.extern_type_decl.name);
            emit_indent(depth);
            // In C, "typedef struct Name Name;" allows using "Name" as an opaque type
            EMIT("typedef struct %s %s;\n", name, name);
            register_struct_type(name); 
            break;
        }

        case DECL_TYPE_ALIAS: {
            // Refinement aliases like `type Pressure = i32 >= 0 and <= 1000`
            // get a typedef so downstream usage can name them. May have
            // been pre-emitted in the forward stage (see emit.h); skip
            // duplicates via is_struct_type().
            Expr *rhs = decl->as.type_alias_decl.expr;
            if (rhs && rhs->kind == EXPR_TYPE && rhs->as.type_expr.type_value) {
                Type *under = rhs->as.type_expr.type_value;
                if (under->kind == TYPE_SIMPLE && under->base_type) {
                    const char *alias_cn = c_name_for_id(decl->as.type_alias_decl.name);
                    if (!is_struct_type(alias_cn)) {
                        char back[128];
                        c_name_for_type(under, back, sizeof back);
                        emit_indent(depth);
                        EMIT("typedef %s %s;\n", back, alias_cn);
                        register_scalar_typedef(alias_cn);
                    }
                }
            }
            break;
        }

        case DECL_IMPORT:
            // Imports are handled by frontend resolution, nothing to emit directly in C
            // (unless we decide to emit #include "module.h" later, but for now single file/unity build assumed or managed externally)
            break;
            
        case DECL_DESTRUCT:
            break; // handled in function params

        default:
            emit_indent(depth);
            EMIT("/* Unhandled declaration type %d */\n", decl->kind);
            break;
    }
}

#endif // EMIT_DECL_H
