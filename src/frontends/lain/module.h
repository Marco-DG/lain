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

// The program's own file, as the driver was given it. Its module name is derived from the path
// (main.c's filepath_to_modname) but cannot always be turned back into it, so the root is read
// from here; an imported module is found from its dotted name.
static const char *module_root_file = NULL;

// Registry of import qualifiers: (importer, qualifier, module) triples, the qualifier being the
// alias or the module path's last segment. Populated during load (the DECL_IMPORT nodes are
// spliced out afterward). ★ A qualifier is the IMPORTER'S: it was one global list, so a program
// importing only std.io could write `c.printf`, because std.io imports std.c.
typedef struct QualifierNode {
    char *importer;   // the module whose import declared it
    char *name;       // the qualifier
    char *module;     // the module it names
    struct QualifierNode *next;
} QualifierNode;
static QualifierNode *import_qualifiers = NULL;

static void register_qualifier(Arena *arena, const char *importer, const char *name, size_t len,
                               const char *module) {
    for (QualifierNode *q = import_qualifiers; q; q = q->next)
        if (strcmp(q->importer, importer) == 0 && strlen(q->name) == len &&
            strncmp(q->name, name, len) == 0) return;
    QualifierNode *q = arena_push_aligned(arena, QualifierNode);
    size_t il = strlen(importer) + 1, ml = strlen(module) + 1;
    q->importer = arena_push_many(arena, char, (isize)il); memcpy(q->importer, importer, il);
    q->name = arena_push_many(arena, char, (isize)len + 1);
    memcpy(q->name, name, len); q->name[len] = '\0';
    q->module = arena_push_many(arena, char, (isize)ml); memcpy(q->module, module, ml);
    q->next = import_qualifiers; import_qualifiers = q;
}
// The module that `name` qualifies in module `importer`, or NULL.
static const char *qualifier_module(const char *importer, const char *name, size_t len) {
    if (!importer) return NULL;
    for (QualifierNode *q = import_qualifiers; q; q = q->next)
        if (strcmp(q->importer, importer) == 0 && strlen(q->name) == len &&
            strncmp(q->name, name, len) == 0)
            return q->module;
    return NULL;
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

// The import graph, kept so that each selective import can be checked once every module is
// loaded. Checked at the import itself, the verdict depended on load order: a module that is
// already loaded is not spliced again, so after `import std.c.{fopen}` std.io's list no longer
// held std.c's names, and `import std.io.{printf}` was refused where the same import alone
// was accepted.
typedef struct ImportEdge {
    char *from, *to;          // importing and imported module paths
    Decl *import;             // the import declaration (its selected names and position)
    char *file;               // the importing module's file
    struct ImportEdge *next;
} ImportEdge;
static ImportEdge *import_edges = NULL, **import_edges_tail = &import_edges;

static char *module_arena_copy(Arena *arena, const char *s) {
    size_t n = strlen(s) + 1;
    char *c = arena_push_many(arena, char, (isize)n);
    memcpy(c, s, n);
    return c;
}

static void record_import_edge(Arena *arena, const char *from, const char *to, Decl *import,
                               const char *file) {
    ImportEdge *e = arena_push_aligned(arena, ImportEdge);
    e->from = module_arena_copy(arena, from);
    e->to = module_arena_copy(arena, to);
    e->file = module_arena_copy(arena, file);
    e->import = import;
    e->next = NULL;
    *import_edges_tail = e; import_edges_tail = &e->next;
}

// Whether `d` defines `name` at module scope (a function, a type, a constant).
static bool decl_defines_name(Decl *d, Id *name) {
    Id *n = NULL;
    switch (d->kind) {
        case DECL_VARIABLE:        n = d->as.variable_decl.name; break;
        case DECL_FUNCTION:
        case DECL_EXTERN_FUNCTION: n = d->as.function_decl.name; break;
        case DECL_STRUCT:          n = d->as.struct_decl.name; break;
        case DECL_ENUM:            n = d->as.enum_decl.type_name; break;
        case DECL_EXTERN_TYPE:     n = d->as.extern_type_decl.name; break;
        case DECL_TYPE_ALIAS:      n = d->as.type_alias_decl.name; break;
        default: break;
    }
    return n && n->length == name->length && memcmp(n->name, name->name, (size_t)n->length) == 0;
}

// Whether `name` is defined by module `mod` or by a module it imports, directly or not.
// `seen` holds the modules already searched (an import cycle is legal).
static bool module_reaches_name(DeclList *program, const char *mod, Id *name,
                                const char **seen, size_t *nseen, size_t cap) {
    for (size_t k = 0; k < *nseen; k++) if (strcmp(seen[k], mod) == 0) return false;
    if (*nseen == cap) return false;
    seen[(*nseen)++] = mod;
    for (DeclList *dl = program; dl; dl = dl->next)
        if (dl->decl && dl->decl->defining_module && strcmp(dl->decl->defining_module, mod) == 0 &&
            decl_defines_name(dl->decl, name))
            return true;
    for (ImportEdge *e = import_edges; e; e = e->next)
        if (strcmp(e->from, mod) == 0 && module_reaches_name(program, e->to, name, seen, nseen, cap))
            return true;
    return false;
}

// For a diagnostic about `q.name` where `q` is not a value: the qualifier module `importer`
// names module `mod` by (the last segment of its path, or the alias it is imported under), or
// NULL when `importer` does not import `mod`.
static const char *module_qualifier_of(const char *importer, const char *mod, size_t *len) {
    if (!importer) return NULL;
    for (QualifierNode *q = import_qualifiers; q; q = q->next)
        if (strcmp(q->importer, importer) == 0 && strcmp(q->module, mod) == 0) {
            *len = strlen(q->name); return q->name;
        }
    return NULL;
}

// Whether module `from` reaches module `target`: it is `target`, or imports it, directly or not.
static bool module_reaches_module(const char *from, const char *target) {
    if (module_paths_equal(from, target)) return true;
    size_t cap = 0;
    for (ModuleNode *n = loaded_modules; n; n = n->next) cap++;
    const char **queue = malloc((cap ? cap : 1) * sizeof *queue);
    size_t head = 0, tail = 0; bool found = false;
    queue[tail++] = from;
    while (head < tail && !found) {
        const char *m = queue[head++];
        for (ImportEdge *e = import_edges; e && !found; e = e->next) {
            if (strcmp(e->from, m) != 0) continue;
            if (module_paths_equal(e->to, target)) { found = true; break; }
            bool queued = false;
            for (size_t k = 0; k < tail; k++) if (strcmp(queue[k], e->to) == 0) { queued = true; break; }
            if (!queued && tail < cap) queue[tail++] = e->to;
        }
    }
    free(queue);
    return found;
}

// A loaded module whose path is `head.next`, or begins with it: `std.math` for `std` and `math`.
static const char *module_with_path_head(const char *head, size_t hl, const char *next, size_t nl) {
    for (ModuleNode *n = loaded_modules; n; n = n->next) {
        const char *p = n->name;
        if (strlen(p) >= hl + 1 + nl && strncmp(p, head, hl) == 0 && p[hl] == '.' &&
            strncmp(p + hl + 1, next, nl) == 0 && (p[hl + 1 + nl] == '\0' || p[hl + 1 + nl] == '.'))
            return p;
    }
    return NULL;
}

// A loaded module whose path ends in the segment `seg`: `std.math` for `math`.
static const char *module_with_last_segment(const char *seg, size_t sl) {
    for (ModuleNode *n = loaded_modules; n; n = n->next) {
        const char *dot = strrchr(n->name, '.');
        const char *last = dot ? dot + 1 : n->name;
        if (strlen(last) == sl && strncmp(last, seg, sl) == 0) return n->name;
    }
    return NULL;
}

// ★ A SELECTED NAME MUST EXIST where it is imported from (Handwriting, M12): `import
// std.math.{mxa}` was accepted, so a typo surfaced later, at the use, as an unrelated error, or
// never if the name went unused. Imports share one namespace, so a name the module reaches
// through its own imports counts; a name defined nowhere in reach is refused at the import.
static void module_check_selected_imports(DeclList *program) {
    size_t cap = 0;
    for (ModuleNode *n = loaded_modules; n; n = n->next) cap++;
    const char **seen = malloc((cap ? cap : 1) * sizeof *seen);
    for (ImportEdge *e = import_edges; e; e = e->next) {
        for (IdList *sn = e->import->as.import_decl.selected; sn; sn = sn->next) {
            size_t nseen = 0;
            if (module_reaches_name(program, e->to, sn->id, seen, &nseen, cap)) continue;
            fprintf(stderr, "[E106] Error Ln %li, Col %li: module '%s' defines no '%.*s' to "
                    "import.\n", (long)e->import->line, (long)e->import->col, e->to,
                    (int)sn->id->length, sn->id->name);
            fprintf(stderr, "  --> %s:%li:%li\n", e->file, (long)e->import->line, (long)e->import->col);
            exit(1);
        }
    }
    free(seen);
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
///   ast_arena:  used only for building AST nodes.
/// Each source file has its own allocation (file_read_source).
static DeclList* load_module(Arena *ast_arena,
                             const char *modname)
{
    if (module_already_loaded(modname)) {
        return NULL;
    }
    bool root = loaded_modules == NULL;   // the program's own module: the last to finish loading

    // 1) build the filesystem path
    char path[256];
    if (root && module_root_file) {
        if (strlen(module_root_file) >= sizeof path) {   // never read a truncated path
            fprintf(stderr, "lain: the path '%s' is too long.\n", module_root_file);
            exit(1);
        }
        snprintf(path, sizeof path, "%s", module_root_file);
    } else module_name_to_path(modname, path, sizeof path);

    // 2) read the file
    File f = file_read_source(path);
    if (!f.contents) {   // the program's own file: an imported module is checked at its import
        fprintf(stderr, "lain: cannot open '%s'.\n", path);
        exit(1);
    }

    // 3) lex + parse into ast_arena
    Lexer   lex    = lexer_new(f.contents);
    {   // a parse error names this file and shows its line (parser/core.h); the copy outlives `path`
        size_t pl = strlen(path) + 1;
        char *pc = arena_push_many_aligned(ast_arena, char, pl);
        memcpy(pc, path, pl);
        parser_file = pc; parser_src = f.contents;
    }
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
                register_qualifier(ast_arena, modname, alias->name, (size_t)alias->length, buf);
            } else {
                const char *seg = buf; size_t seglen = strlen(buf);
                const char *dot = strrchr(buf, '.');
                if (dot) { seg = dot + 1; seglen = strlen(dot + 1); }
                register_qualifier(ast_arena, modname, seg, seglen, buf);
            }
            // Selective imports: `import M.{a, b}` brings a, b unqualified into
            // the importing module (`modname`).
            for (IdList *sn = cur->decl->as.import_decl.selected; sn; sn = sn->next)
                register_sel_import(ast_arena, modname, sn->id);

            record_import_edge(ast_arena, modname, buf, cur->decl, path);

            // A module that does not exist is refused at the import that names it, with its
            // position; it was an uncoded "Error: Cannot open module file" with neither.
            if (!module_already_loaded(buf)) {
                char mpath[256];
                module_name_to_path(buf, mpath, sizeof mpath);
                FILE *probe = fopen(mpath, "rb");
                if (!probe) {
                    fprintf(stderr, "[E106] Error Ln %li, Col %li: there is no module '%s' (no file "
                            "'%s').\n", (long)cur->decl->line, (long)cur->decl->col, buf, mpath);
                    fprintf(stderr, "  --> %s:%li:%li\n", path, (long)cur->decl->line,
                            (long)cur->decl->col);
                    exit(1);
                }
                fclose(probe);
            }

            // recurse
            DeclList *child = load_module(ast_arena, buf);
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
    if (root) module_check_selected_imports(decls);
    return decls;
}

#endif // MODULE_H
