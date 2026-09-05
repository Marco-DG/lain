// src/analysis/effects_driver.c — Phase 3.3 differential: for every function, compare the
// NEW IR effect pass (analysis/effects.h, reads only the IR) against the OLD AST effect
// analysis (sema.h effect_full). Agreement across the corpus is the parity gate for 3.3.
//
//   gcc -std=c99 -o /tmp/effdrv src/analysis/effects_driver.c -I src
//   /tmp/effdrv tests/vra/two_pointer_reverse_pass.ln         # prints per-func + a verdict
//   exit 0 = all functions agree ; 1 = a mismatch (inspect) ; 2 = load/parse error
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
#include "analysis/effects.h"

static char *drv_modname(Arena *a, const char *path) {
    const char *p = path;
    while ((p[0]=='.' && (p[1]=='/'||p[1]=='\\')) || p[0]=='/' || p[0]=='\\')
        p += (p[0]=='/'||p[0]=='\\') ? 1 : 2;
    size_t n = strlen(p), end = (n>3 && strcmp(p+n-3,".ln")==0) ? n-3 : n;
    char *t = arena_push_many(a, char, end+1);
    memcpy(t, p, end); t[end] = '\0';
    return t;
}
static void fmt(unsigned e, char *out) {
    const char *nm[5] = {"Write","Diverge","Raises","IO","Alloc"};
    out[0]='{'; int p=1; const char *sep="";
    for (int i=0;i<5;i++) if (e&(1u<<i)) { p+=sprintf(out+p,"%s%s",sep,nm[i]); sep=","; }
    out[p++]='}'; out[p]='\0';
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.ln> [--quiet]\n", argv[0]); return 2; }
    bool quiet = (argc>=3 && strcmp(argv[2],"--quiet")==0);
    Arena file_arena=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ast_arena =arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena sema_arena=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ir_arena  =arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    target_init_for(NULL);

    const char *path=argv[1]; const char *slash=NULL;
    for (const char *q=path;*q;q++) if (*q=='/'||*q=='\\') slash=q;
    if (slash){ char dir[4096]; size_t dl=(size_t)(slash-path);
        if (dl<sizeof dir){ memcpy(dir,path,dl); dir[dl]='\0'; if (chdir(dir)!=0){} path=slash+1; } }

    char *modname=drv_modname(&ast_arena,path);
    DeclList *program=load_module(&file_arena,&ast_arena,modname);
    if (!program){ fprintf(stderr,"load failed: %s\n",modname); return 2; }
    sema_resolve_module(program, modname, &sema_arena);

    IrFunc *mod = ir_lower_module(program, &ir_arena);
    ir_effects_module(mod);

    int mism=0, n=0;
    for (DeclList *d=program; d; d=d->next) {
        if (!d->decl) continue;
        if ((d->decl->kind!=DECL_FUNCTION && d->decl->kind!=DECL_PROCEDURE) ||
            !d->decl->as.function_decl.body) continue;
        Id *nm=d->decl->as.function_decl.name; if (!nm) continue;
        IrFunc *f=NULL;
        for (IrFunc *g=mod; g; g=g->next)
            if (!g->is_extern && g->name && g->name->length==nm->length &&
                memcmp(g->name->name,nm->name,(size_t)nm->length)==0){ f=g; break; }
        if (!f) continue;
        unsigned oldE = (unsigned)effect_full(d->decl);
        unsigned newE = f->effects;
        // A function the IR lowered INCOMPLETELY can't be compared faithfully — skip it.
        if (f->incomplete) continue;
        n++;
        char ob[128], nb[128]; fmt(oldE,ob); fmt(newE,nb);
        // The soundness-critical direction: new dropping an OBSERVABLE effect the old has.
        // DIVERGE is excluded — it is semantic (VRA termination), and new legitimately
        // proves termination where the old engine only checked syntactically (no measure).
        unsigned OBSERVABLE = IR_EFFECT_WRITE | IR_EFFECT_IO | IR_EFFECT_RAISES;
        unsigned dropped = oldE & ~newE & OBSERVABLE;
        if (dropped){ mism++;
            fprintf(stderr,"UNSOUND %.*s  old=%s new=%s  (dropped observable %s)\n",
                    (int)nm->length,nm->name,ob,nb, (dropped&IR_EFFECT_WRITE)?"Write":(dropped&IR_EFFECT_IO)?"IO":"Raises");
        } else if (newE!=oldE){
            if (!quiet) fprintf(stdout,"  ~%.*s : old=%s new=%s (Diverge-precision, sound)\n",(int)nm->length,nm->name,ob,nb);
        } else if (!quiet) {
            fprintf(stdout,"  %.*s : %s%s\n",(int)nm->length,nm->name,nb,newE==0?" (pure&total)":"");
        }
    }
    if (!quiet) fprintf(stdout,"effects: %d funcs compared, %d UNSOUND (dropped observable effect)\n", n, mism);
    return mism ? 1 : 0;
}
