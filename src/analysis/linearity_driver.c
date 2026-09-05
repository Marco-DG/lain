// src/analysis/linearity_driver.c — Phase 3.2 differential harness for the IR linearity
// pass. Prints E001/E002 findings per function; exit 1 if any (i.e. the new engine would
// REJECT), 0 if clean (ACCEPT). Compare accept/reject to the old engine over the ownership
// corpus:  pass-tests must stay clean (no false positive); move-error fail-tests must fire.
//
//   gcc -std=c99 -o /tmp/lindrv src/analysis/linearity_driver.c -I src
//   /tmp/lindrv tests/ownership/use_after_move_fail.ln
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
#include "analysis/linearity.h"

static char *drv_modname(Arena *a, const char *path) {
    const char *p = path;
    while ((p[0]=='.' && (p[1]=='/'||p[1]=='\\')) || p[0]=='/' || p[0]=='\\')
        p += (p[0]=='/'||p[0]=='\\') ? 1 : 2;
    size_t n = strlen(p), end = (n>3 && strcmp(p+n-3,".ln")==0) ? n-3 : n;
    char *t = arena_push_many(a, char, end+1); memcpy(t,p,end); t[end]='\0'; return t;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"usage: %s <file.ln> [--quiet]\n",argv[0]); return 2; }
    bool quiet = (argc>=3 && strcmp(argv[2],"--quiet")==0);
    Arena fa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena aa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena sa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ia=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
    target_init_for(NULL);
    const char *path=argv[1]; const char *slash=NULL;
    for (const char *q=path;*q;q++) if (*q=='/'||*q=='\\') slash=q;
    if (slash){ char dir[4096]; size_t dl=(size_t)(slash-path);
        if (dl<sizeof dir){ memcpy(dir,path,dl); dir[dl]='\0'; if(chdir(dir)!=0){} path=slash+1; } }
    char *mod=drv_modname(&aa,path);
    DeclList *program=load_module(&fa,&aa,mod);
    if (!program){ fprintf(stderr,"load failed: %s\n",mod); return 2; }
    sema_resolve_module(program, mod, &sa);

    IrFunc *m = ir_lower_module(program, &ia);
    int total=0;
    for (IrFunc *f=m; f; f=f->next) {
        if (f->is_extern || f->incomplete) continue;   // can't judge an unfaithful lowering
        Lin *L = lin_analyze(f);
        for (int i=0;i<L->nfinds;i++) { total++;
            const char *tag = L->finds[i].code==1?"E001 use-after-move"
                            : L->finds[i].code==2?"E002 double-move":"E003 leak";
            const char *msg = L->finds[i].code==1?"use of moved value"
                            : L->finds[i].code==2?"moved twice":"linear value not consumed";
            fprintf(stderr,"[%s] %.*s: %s (slot %%%d)\n", tag,
                f->name?(int)f->name->length:1, f->name?f->name->name:"?", msg, L->finds[i].slot);
        }
        lin_free(L);
    }
    if (!quiet) fprintf(stdout,"linearity: %d finding(s) %s\n", total, total?"(REJECT)":"(accept)");
    return total ? 1 : 0;
}
