#ifndef MODULE_H
#define MODULE_H

#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "utils/file.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* when you use the flag "-std=c99" the compiler hides all non-standard function in the header "string.h" */
extern char* strdup(const char*);

#include "utils/file.h"
#include <string.h>    // for strdup, memcpy
#include <stdlib.h>    // for malloc, free, exit

/*──────────────────────────────────────────────────────────────────╗
│ A linked list of loaded modules (so we never load twice).
╚──────────────────────────────────────────────────────────────────*/
typedef struct ModuleNode {
    char             *name;    // e.g. "foo.bar"
    DeclList         *decls;   // AST of that module
    struct ModuleNode *next;
} ModuleNode;

static ModuleNode *loaded_modules = NULL;

static bool module_already_loaded(const char *name) {
    for (ModuleNode *n = loaded_modules; n; n = n->next)
        if (strcmp(n->name, name) == 0)
            return true;
    return false;
}

static void record_module(const char *name, DeclList *decls) {
    ModuleNode *n = malloc(sizeof *n);
    n->name  = strdup(name);
    n->decls = decls;
    n->next  = loaded_modules;
    loaded_modules = n;
}

/// “foo.bar.baz” → “foo/bar/baz.ln”
static void module_name_to_path(const char *mod, char *out, size_t cap) {
    size_t i = 0;
    for (const char *p = mod; *p && i+1 < cap; p++) {
        out[i++] = (*p == '.') ? '/' : *p;
    }
    const char *ext = ".ln";
    for (size_t j = 0; ext[j] && i+1 < cap; j++) {
        out[i++] = ext[j];
    }
    out[i] = '\0';
}

/// Load (and splice) a module into the AST‐arena.
///   file_arena: used only for reading files,
///   ast_arena:  used only for building AST nodes.
static DeclList* load_module(Arena *file_arena,
                             Arena *ast_arena,
                             const char *modname)
{
    if (module_already_loaded(modname)) {
        return NULL;
    }

    // 1) build the filesystem path
    char path[256];
    module_name_to_path(modname, path, sizeof path);

    // 2) read the file into file_arena
    File f = file_read_into_arena(file_arena, path);
    if (!f.contents) {
        fprintf(stderr, "Error: Cannot open module file '%s'\n", path);
        exit(1);
    }

    // 3) lex + parse into ast_arena
    Lexer   lex    = lexer_new(f.contents);
    Parser  parser = {
      .lexer  = &lex,
      .line   = 1,
      .column = 1
    };
    _parser_advance(&parser); // Fetch first token (and normalize NEWLINE -> EOL)
    DeclList *decls = parse_module(ast_arena, &parser);
    // 4) splice any imports in this module
    DeclList *prev = NULL, *cur = decls;
    while (cur) {
        if (cur->decl->kind == DECL_IMPORT) {
            Id *imp       = cur->decl->as.import_decl.module_name;
            size_t len    = imp->length;
            char buf[256];
            if (len >= sizeof buf) len = sizeof buf - 1;
            memcpy(buf, imp->name, len);
            buf[len] = '\0';

            // recurse
            DeclList *child = load_module(file_arena, ast_arena, buf);
            if (child) {
                // splice child in place of this import
                DeclList *end = child;
                while (end->next) end = end->next;

                if (prev) prev->next = child;
                else       decls     = child;

                end->next = cur->next;
                cur = end->next;
                continue;
            }
        }
        prev = cur;
        cur  = cur->next;
    }

    // 5) record & return
    record_module(modname, decls);
    return decls;
}

#endif // MODULE_H
