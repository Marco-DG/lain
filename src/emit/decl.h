#ifndef EMIT_DECL_H
#define EMIT_DECL_H

#include "../emit.h"

// Emit one top-level declaration
void emit_decl(Decl *decl, int depth);
static void emit_forward_decl(Decl *decl, int depth);
// Emit a list of declarations (the whole program)
void emit_decl_list(DeclList *decls, int depth);

static void emit_param_type(Type *t) {
    if (!t) return;
    
    // Get the base type name without ownership decorations,
    // BUT for Pointers, the mode dictates the C type (const vs non-const),
    // so we must preserve it to generate the correct base "value type".
    OwnershipMode original_mode = t->mode;
    if (t->kind != TYPE_POINTER) {
        t->mode = MODE_SHARED;
    }
    
    char base_name[128];
    c_name_for_type(t, base_name, sizeof(base_name));
    
    t->mode = original_mode;  // restore original mode
    
    // Now emit the correct C type based on ownership mode
    if (original_mode == MODE_OWNED) {
        // mov T -> pass by value (T)
        EMIT("%s", base_name);
    } else if (original_mode == MODE_MUTABLE) {
        // mut T -> pass as mutable pointer (T*)
        EMIT("%s *", base_name);
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
                          emit_param_type(param->decl->as.destruct_decl.type);
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
                              emit_param_type(pt);
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
            emit_indent(depth);
            // Print return type and function name.
            if (decl->as.function_decl.return_type) {
                emit_type(decl->as.function_decl.return_type);
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
                    if (!first) {
                        EMIT(", ");
                    }
                    
                    if (param->decl->kind == DECL_DESTRUCT) {
                        // Emit: Type _param_N
                        emit_param_type(param->decl->as.destruct_decl.type);
                        EMIT(" _param_%d", param_idx);
                    } else {
                        // Use emit_param_type to print parameter type.
                        emit_param_type(param->decl->as.variable_decl.type);
                        EMIT(" %.*s",
                                (int)param->decl->as.variable_decl.name->length,
                                param->decl->as.variable_decl.name->name);
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
            EMIT("}\n\n");
            break;
        }


        case DECL_STRUCT: {
            // 1) struct definition
            emit_indent(depth);
            const char *structName = c_name_for_id(decl->as.struct_decl.name);
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

            // 2) Generate the Tag Enum: typedef enum { Shape_Tag_Circle, Shape_Tag_Rectangle } Shape_Tag;
            emit_indent(depth);
            EMIT("typedef enum {\n");
            
            for (Variant *v = decl->as.enum_decl.variants; v; v = v->next) {
                emit_indent(depth + 1);
                EMIT("%s_Tag_%.*s,\n", adt_name, (int)v->name->length, v->name->name);
            }
            
            emit_indent(depth);
            EMIT("} %s_Tag;\n\n", adt_name);

            // 3) Generate the ADT Struct: typedef struct { Shape_Tag tag; union { ... } data; } Shape;
            emit_indent(depth);
            EMIT("typedef struct {\n");
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
