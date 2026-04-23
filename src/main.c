#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/file.h"
#include "utils/common/system.h"
#include "utils/panic.h"

#include <unistd.h> /* chdir */

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "ast_print.h"
#include "module.h"
#include "emit.h"
#include "error.h"
#include "args.h"
#include "sema.h"

void expr_print_ast(Expr *expr, int depth);
void stmt_print_ast(Stmt *stmt, int depth);
void decl_print_ast(Decl *decl, int depth);
void print_ast(DeclList* decl_list, int depth); // Function prototype

// utility: turn "foo/bar/baz.ln" or "./foo/bar.ln" or "/foo/bar.ln"
//          → "foo.bar.baz"
static char *filepath_to_modname(Arena *arena, const char *path) {
    // 1) skip any leading "./", ".\", "/" or "\"
    const char *p = path;
    while ((p[0] == '.' && (p[1] == '/' || p[1] == '\\')) 
        || p[0] == '/' || p[0] == '\\') {
        if (p[0] == '/' || p[0] == '\\') {
            p += 1;
        } else {
            p += 2;  // skip "./" or ".\"
        }
    }

    // 2) determine length without the ".ln" extension
    size_t n = strlen(p);
    size_t end = (n > 3 && strcmp(p + n - 3, ".ln") == 0)
                 ? n - 3
                 : n;

    // 3) allocate exactly end+1 chars in the AST arena
    char *tmp = arena_push_many(arena, char, end + 1);
    for (size_t i = 0; i < end; i++) {
        char c = p[i];
        tmp[i] = (c == '/' || c == '\\') ? '.' : c;
    }
    tmp[end] = '\0';
    return tmp;
}


int main(int argc, char **argv) {
    // two arenas:
    // two arenas:
    Arena file_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*1024);
    Arena ast_arena  = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*1024);
    Arena _sema_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*1024);

    Args args = args_parse(argc, argv);

    // C.1 fix: if the user passed an **absolute** path, chdir to its directory
    // so import-based module resolution keeps working. Relative paths are left
    // untouched — the project convention is to invoke lain from the repo root
    // with a relative path so that std/ resolves correctly.
    if (args.filename && args.filename[0] == '/') {
        const char *fname = args.filename;
        const char *last_sep = NULL;
        for (const char *p = fname; *p; p++) {
            if (*p == '/' || *p == '\\') last_sep = p;
        }
        if (last_sep && last_sep != fname) {
            size_t dirlen = (size_t)(last_sep - fname);
            char dirbuf[4096];
            if (dirlen < sizeof(dirbuf)) {
                memcpy(dirbuf, fname, dirlen);
                dirbuf[dirlen] = '\0';
                if (chdir(dirbuf) != 0) {
                    fprintf(stderr, "Error: cannot chdir to '%s' for module resolution.\n", dirbuf);
                    return 1;
                }
                args.filename = (char *)(last_sep + 1);
            }
        }
    }

    // derive a nice “foo.bar” module‑name from the filename
    // (strip “.ln” and turn “/” into “.”)
    char *modname = filepath_to_modname(&ast_arena, args.filename);

    DeclList *program = load_module(&file_arena, &ast_arena, modname);
    if (!program) {
        fprintf(stderr, "Could not load root module %s\n", modname);
        return 1;
    }

    if (args.dump_ast) {
        printf("\n\n#### AST ####\n");
        print_ast(program, 0);
        return 0;
    }

    // sema = resolve identifiers → you’d call:
    sema_resolve_module(program, modname, &_sema_arena);

    // then code-gen:
    emit_source_filename = args.filename;
    emit(program, 0, args.output_file);

    sema_destroy();

    return 0;
}