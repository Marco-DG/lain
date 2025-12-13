#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/file.h"
#include "utils/common/system.h"
#include "utils/panic.h"

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
    Arena file_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*128);
    Arena ast_arena  = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*128);
    Arena _sema_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*128);

    Args args = args_parse(argc, argv);

    // derive a nice “foo.bar” module‑name from the filename
    // (strip “.ln” and turn “/” into “.”)
    char *modname = filepath_to_modname(&ast_arena, args.filename);

    DeclList *program = load_module(&file_arena, &ast_arena, modname);
    if (!program) {
        fprintf(stderr, "Could not load root module %s\n", modname);
        return 1;
    }

    //printf("\n\n#### AST ####\n");
    //print_ast(program, 0); // Start printing the AST from the root, with depth 0

    // sema = resolve identifiers → you’d call:
    sema_resolve_module(program, modname, &_sema_arena);

    // then code‑gen:
    emit(program, 0, "../out/main.c");

    sema_destroy();

    return 0;
}