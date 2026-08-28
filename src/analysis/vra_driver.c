// src/analysis/vra_driver.c — run the new octagon VRA over a Lain file and report,
// for every array/slice index, whether 0 ≤ idx < len was PROVEN. The first
// end-to-end demonstration of the rebuilt proof engine.
//
//   gcc -std=c99 -o /tmp/vradrv src/analysis/vra_driver.c -I src
//   /tmp/vradrv tests/ir/loopidx.ln
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
#include "analysis/vra.h"

static char *drv_modname(Arena *a, const char *path) {
    const char *p = path;
    while ((p[0]=='.' && (p[1]=='/'||p[1]=='\\')) || p[0]=='/' || p[0]=='\\')
        p += (p[0]=='/'||p[0]=='\\') ? 1 : 2;
    size_t n = strlen(p), end = (n>3 && strcmp(p+n-3,".ln")==0) ? n-3 : n;
    char *t = arena_push_many(a, char, end+1); memcpy(t,p,end); t[end]='\0'; return t;
}

int main(int argc, char **argv) {
    if (argc<2){ fprintf(stderr,"usage: %s <file.ln> [--dump] [--suppress]\n", argv[0]); return 2; }
    bool dump=false, suppress=false;
    for (int k=2;k<argc;k++){ if(!strcmp(argv[k],"--dump"))dump=true; if(!strcmp(argv[k],"--suppress"))suppress=true; }
    Arena fa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena aa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena sa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ia=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    target_init_for(NULL);
    const char *path=argv[1]; const char *slash=NULL;
    for (const char *q=path;*q;q++) if(*q=='/'||*q=='\\') slash=q;
    if (slash){ char dir[4096]; size_t dl=(size_t)(slash-path);
        if(dl<sizeof dir){ memcpy(dir,path,dl); dir[dl]='\0'; if(chdir(dir)!=0){} path=slash+1; } }
    char *mod=drv_modname(&aa,path);
    DeclList *prog=load_module(&fa,&aa,mod);
    if(!prog){ fprintf(stderr,"load failed\n"); return 1; }
    g_vra_suppress_bounds = suppress;   // analyze even legacy-rejected programs (reject-side)
    sema_resolve_module(prog,mod,&sa);

    int total=0, proven=0;
    for (DeclList *d=prog; d; d=d->next) {
        if(!d->decl) continue;
        if((d->decl->kind==DECL_FUNCTION||d->decl->kind==DECL_PROCEDURE) && d->decl->as.function_decl.body){
            IrFunc *f=ir_lower_function(d->decl, prog, &ia);
            if (dump){ ir_dump_func(f, stdout); fputc('\n', stdout); }
            Vra *V=vra_analyze(f);
            for (int i=0;i<V->nchecks;i++){
                VraCheck *c=&V->checks[i]; total++;
                if(c->ok) proven++;
                const char *what = c->kind==VRA_BOUNDS?"index bounds"
                                 : c->kind==VRA_OVERFLOW?"arith overflow" : "div-by-zero";
                printf("  %-8.*s  %-14s @ %lld:%lld  %s\n",
                    (int)f->name->length, f->name->name, what,
                    (long long)c->line,(long long)c->col,
                    c->ok?"PROVEN check-free":"NOT proven");
            }
            vra_free(V);
        }
    }
    printf("%d/%d proof obligations discharged check-free\n", proven, total);
    return 0;
}
