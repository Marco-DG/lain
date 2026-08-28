// src/ir/lower_driver.c — standalone driver: parse + typecheck a Lain file with the
// existing frontend, then LOWER each function to IR and dump it (Phase 1.2 harness).
//
// Reuses the real frontend (load_module → sema_resolve_module gives a TYPED AST), so
// this exercises lowering on genuine programs. Not part of the main build.
//
// Build & run:
//   gcc -std=c99 -o /tmp/lowerdrv src/ir/lower_driver.c -I src
//   /tmp/lowerdrv tests/ir/sum_maxi.ln
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/file.h"
#include "utils/common/system.h"
#include "utils/panic.h"
#include <unistd.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "module.h"
#include "target.h"
#include "sema.h"
#include "ir/lower.h"
#include "ir/dump.h"
#include "ir/emit_c.h"

// module name = basename without ".ln" (mirrors main.c's private helper)
static char *drv_modname(Arena *a, const char *path) {
    const char *p = path;
    while ((p[0]=='.' && (p[1]=='/'||p[1]=='\\')) || p[0]=='/' || p[0]=='\\')
        p += (p[0]=='/'||p[0]=='\\') ? 1 : 2;
    size_t n = strlen(p), end = (n>3 && strcmp(p+n-3,".ln")==0) ? n-3 : n;
    char *t = arena_push_many(a, char, end+1);
    memcpy(t, p, end); t[end] = '\0';
    return t;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.ln>\n", argv[0]); return 2; }
    Arena file_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ast_arena  = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena sema_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ir_arena   = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    target_init_for(NULL);

    // chdir to the file's directory so module resolution (std/, siblings) works;
    // use the basename as the module name.
    const char *path = argv[1]; const char *slash = NULL;
    for (const char *q = path; *q; q++) if (*q=='/'||*q=='\\') slash = q;
    if (slash) { char dir[4096]; size_t dl=(size_t)(slash-path);
        if (dl<sizeof dir){ memcpy(dir,path,dl); dir[dl]='\0'; if (chdir(dir)!=0){} path=slash+1; } }

    char *modname = drv_modname(&ast_arena, path);
    DeclList *program = load_module(&file_arena, &ast_arena, modname);
    if (!program) { fprintf(stderr, "load failed: %s\n", modname); return 1; }
    sema_resolve_module(program, modname, &sema_arena);

    bool emit_c = (argc >= 3 && strcmp(argv[2], "--emit-c") == 0);
    IrFunc *head = NULL, *tail = NULL;
    for (DeclList *d = program; d; d = d->next) {
        if (!d->decl) continue;
        if ((d->decl->kind == DECL_FUNCTION || d->decl->kind == DECL_PROCEDURE)
            && d->decl->as.function_decl.body) {
            IrFunc *f = ir_lower_function(d->decl, program, &ir_arena);
            if (!head) head = tail = f; else { tail->next = f; tail = f; }
            if (!emit_c) { ir_dump_func(f, stdout); fputc('\n', stdout); }
        }
    }
    if (emit_c) ir_emit_module_c(head, stdout, &ir_arena);
    return 0;
}
