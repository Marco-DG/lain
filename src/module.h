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
    const char       *source_text; // raw source for diagnostics
    const char       *source_file; // file path for diagnostics
    struct ModuleNode *next;
} ModuleNode;

static ModuleNode *loaded_modules = NULL;

// Registry of import qualifiers (the alias, or a module path's last segment) so
// name resolution can recognize `qualifier.Member` as qualified module access.
// Populated during load (the DECL_IMPORT nodes are spliced out afterward).
typedef struct QualifierNode {
    char *name;
    struct QualifierNode *next;
} QualifierNode;
static QualifierNode *import_qualifiers = NULL;

static void register_qualifier(Arena *arena, const char *name, size_t len) {
    for (QualifierNode *q = import_qualifiers; q; q = q->next)
        if (strlen(q->name) == len && strncmp(q->name, name, len) == 0) return;
    QualifierNode *q = arena_push_aligned(arena, QualifierNode);
    q->name = arena_push_many(arena, char, (isize)len + 1);
    memcpy(q->name, name, len); q->name[len] = '\0';
    q->next = import_qualifiers; import_qualifiers = q;
}
static bool qualifier_is_module(const char *name, size_t len) {
    for (QualifierNode *q = import_qualifiers; q; q = q->next)
        if (strlen(q->name) == len && strncmp(q->name, name, len) == 0) return true;
    return false;
}

// Registry of selective imports: (importer module, name) pairs — the names an
// importer pulled in unqualified via `import M.{a, b}`. With the glob retired,
// a bare cross-module name is visible ONLY if it appears here.
typedef struct SelImportNode {
    char *importer;   // importing module path (defining_module of the import site)
    char *name;       // the unqualified name brought in
    struct SelImportNode *next;
} SelImportNode;
static SelImportNode *sel_imports = NULL;

static void register_sel_import(Arena *arena, const char *importer, Id *name) {
    if (!importer || !name) return;
    SelImportNode *n = arena_push_aligned(arena, SelImportNode);
    size_t il = strlen(importer);
    n->importer = arena_push_many(arena, char, (isize)il + 1);
    memcpy(n->importer, importer, il + 1);
    n->name = arena_push_many(arena, char, (isize)name->length + 1);
    memcpy(n->name, name->name, (size_t)name->length); n->name[name->length] = '\0';
    n->next = sel_imports; sel_imports = n;
}
// Compare two module paths treating '.' and '_' as equal — the dotted form
// (`std.io`) and the C-sanitized form (`std_io`) name the same module.
static bool module_paths_equal(const char *a, const char *b) {
    if (!a || !b) return a == b;
    for (; *a && *b; a++, b++) {
        char ca = (*a == '.') ? '_' : *a;
        char cb = (*b == '.') ? '_' : *b;
        if (ca != cb) return false;
    }
    return *a == *b;
}
static bool sel_import_visible(const char *importer, const char *name, size_t len) {
    if (!importer) return false;
    for (SelImportNode *s = sel_imports; s; s = s->next)
        if (strcmp(s->importer, importer) == 0 &&
            strlen(s->name) == len && strncmp(s->name, name, len) == 0)
            return true;
    return false;
}

static bool module_already_loaded(const char *name) {
    for (ModuleNode *n = loaded_modules; n; n = n->next)
        if (strcmp(n->name, name) == 0)
            return true;
    return false;
}

static ModuleNode* record_module(Arena *arena, const char *name, DeclList *decls, const char *source_text, const char *source_file) {
    // F-057: arena-allocate so the compiler stays consistent with its
    // arena-based ownership model (no leaks, no explicit free).
    ModuleNode *n = arena_push_aligned(arena, ModuleNode);
    size_t name_len = strlen(name) + 1;
    char *name_copy = arena_push_many_aligned(arena, char, name_len);
    memcpy(name_copy, name, name_len);
    n->name  = name_copy;
    n->decls = decls;
    n->source_text = source_text;
    // Arena-copy source_file: callers may pass a stack-local buffer
    // (load_module's `path[256]`), which would dangle after return.
    if (source_file) {
        size_t sf_len = strlen(source_file) + 1;
        char *sf_copy = arena_push_many_aligned(arena, char, sf_len);
        memcpy(sf_copy, source_file, sf_len);
        n->source_file = sf_copy;
    } else {
        n->source_file = NULL;
    }
    n->next  = loaded_modules;
    loaded_modules = n;
    return n;
}

// Lookup a module record by name
static ModuleNode *find_module(const char *name) {
    for (ModuleNode *n = loaded_modules; n; n = n->next)
        if (strcmp(n->name, name) == 0)
            return n;
    return NULL;
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

    // Q-018: tag every decl with its defining module path (for cross-module
    // visibility checks). Use a stable copy of `modname` in ast_arena.
    {
        size_t mn_len = strlen(modname) + 1;
        char *modname_copy = arena_push_many_aligned(ast_arena, char, mn_len);
        memcpy(modname_copy, modname, mn_len);
        for (DeclList *dl = decls; dl; dl = dl->next) {
            if (dl->decl && dl->decl->defining_module == NULL) {
                dl->decl->defining_module = modname_copy;
            }
        }
    }

    // Record this module BEFORE recursing into its imports so a cyclic import
    // (A imports B imports A) sees A as already loaded and stops instead of
    // recursing forever into a stack-overflow crash. The decls pointer is
    // refreshed after splicing (the head can change). Imports are a flat
    // namespace, so a cycle just resolves to a single shared load.
    ModuleNode *self = record_module(ast_arena, modname, decls, f.contents, path);

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

            // Register the access qualifier: the alias, else the path's last
            // segment (`std.math` → `math`). Enables `qualifier.Member` access
            // (the glob still binds bare names, so this is additive).
            Id *alias = cur->decl->as.import_decl.alias;
            if (alias) {
                register_qualifier(ast_arena, alias->name, (size_t)alias->length);
            } else {
                const char *seg = buf; size_t seglen = strlen(buf);
                const char *dot = strrchr(buf, '.');
                if (dot) { seg = dot + 1; seglen = strlen(dot + 1); }
                register_qualifier(ast_arena, seg, seglen);
            }
            // Selective imports: `import M.{a, b}` brings a, b unqualified into
            // the importing module (`modname`).
            for (IdList *sn = cur->decl->as.import_decl.selected; sn; sn = sn->next)
                register_sel_import(ast_arena, modname, sn->id);

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
                prev = end; // Successive imports must be appended to this new tail
                continue;
            }
        }
        prev = cur;
        cur  = cur->next;
    }

    // 5) refresh the record's decls head (splicing above may have changed it)
    //    and return. The module was already registered before the import loop.
    self->decls = decls;
    return decls;
}

#endif // MODULE_H
