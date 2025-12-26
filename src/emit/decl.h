#ifndef EMIT_DECL_H
#define EMIT_DECL_H

#include "../emit.h"

// Emit one top-level declaration
void emit_decl(Decl *decl, int depth);
static void emit_forward_decl(Decl *decl, int depth);
// Emit a list of declarations (the whole program)
void emit_decl_list(DeclList *decls, int depth);

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


        case DECL_PROCEDURE:
        case DECL_FUNCTION: {
            emit_indent(depth);
            // Print return type and function name.
            emit_type(decl->as.function_decl.return_type);

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
                        emit_type(param->decl->as.destruct_decl.type);
                        EMIT(" _param_%d", param_idx);
                    } else {
                        // Use emit_type to print parameter type.
                        emit_type(param->decl->as.variable_decl.type);
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
                                EMIT(" %.*s = _param_%d.%.*s;\n",
                                     (int)n->id->length, n->id->name,
                                     param_idx,
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
            // 1) lookup the C‐enum tag, e.g. "main_Kind" or "main_State"
            const char *enum_tag = c_name_for_id(
                decl->as.enum_decl.type_name);

            // 2) print the typedef header
            emit_indent(depth);
            EMIT("typedef enum %s {\n", enum_tag);

            // 3) each variant: print "%s_%variant"
            for (IdList *vl = decl->as.enum_decl.variants;
                    vl; vl = vl->next)
            {
                if (!vl->id) continue;
                emit_indent(depth + 1);
                EMIT("%s_%.*s",
                    enum_tag,
                    (int)vl->id->length,
                    vl->id->name);
                if (vl->next) EMIT(",");
                EMIT("\n");
            }

            // 4) close the typedef
            emit_indent(depth);
            EMIT("} %s;\n\n", enum_tag);
        }
        break;


        default:
            emit_indent(depth);
            EMIT("/* Unhandled declaration type */\n");
            break;
    }
}

#endif // EMIT_DECL_H
