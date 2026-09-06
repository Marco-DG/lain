// src/analysis/incomplete_driver.c — measure IR FAITHFULNESS (corrective backlog C3).
//
// `IrFunc.incomplete` means lowering dropped or placeholder'd a construct, so every proof
// over that function is suppressed. Left unmeasured it is an escape hatch that silently
// conditions every survey number in the repo. This driver reports, per file, one WHY line
// per incomplete function plus a TOT summary; ir_incomplete_survey.sh aggregates them into
// a ranked work list.
//
//   gcc -std=c99 -o /tmp/incdrv src/analysis/incomplete_driver.c -I src
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
static char *mn(Arena *a, const char *path){ const char *p=path;
  while ((p[0]=='.'&&(p[1]=='/'||p[1]=='\\'))||p[0]=='/'||p[0]=='\\') p+=(p[0]=='/'||p[0]=='\\')?1:2;
  size_t n=strlen(p),e=(n>3&&strcmp(p+n-3,".ln")==0)?n-3:n;
  char *t=arena_push_many(a,char,e+1); memcpy(t,p,e); t[e]='\0'; return t; }
int main(int argc,char**argv){ if(argc<2) return 2;
  g_suppress_ownership=true;
  Arena fa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
  Arena aa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
  Arena sa=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
  Arena ia=arena_new(memory_alloc,MEMORY_PAGE_MINIMUM_SIZE*4096);
  target_init_for(NULL); const char*path=argv[1]; const char*sl=NULL;
  for(const char*q=path;*q;q++) if(*q=='/'||*q=='\\') sl=q;
  // ABSOLUTE paths only — matching main.c. Chdir'ing for a relative path put the process
  // in the test's directory, where `import std.*` cannot resolve, and the survey then
  // silently dropped the file: the denominator excluded every program that uses the stdlib.
  if(sl && path[0]=='/'){ char d[4096]; size_t dl=(size_t)(sl-path); if(dl<sizeof d){memcpy(d,path,dl);d[dl]='\0'; if(chdir(d)!=0){} path=sl+1;} }
  char *m=mn(&aa,path); DeclList*pr=load_module(&fa,&aa,m); if(!pr) return 2;
  sema_resolve_module(pr,m,&sa);
  IrFunc *mod=ir_lower_module(pr,&ia); int tot=0,inc=0;
  for(IrFunc*f=mod;f;f=f->next){ if(f->is_extern) continue; tot++;
    if(f->incomplete){ inc++; printf("WHY %s\n", f->incomplete_why?f->incomplete_why:"(unlabelled)"); } }
  printf("TOT %d %d\n",tot,inc); return 0; }
