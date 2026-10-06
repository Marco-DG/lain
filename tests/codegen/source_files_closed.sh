#!/usr/bin/env bash
# lain reads each source file whole and keeps no descriptor open after: file_read_source never closed
# its handle, so a compile held one descriptor per source file (the program and every module it
# imports) until it exited (the Debugger agent's finding, which embeds the compiler). Counted here in
# a driver that embeds main.c: the descriptors open after compiling tests/stdlib/mem_smoke_pass.ln,
# which imports std modules, must not outnumber those before it.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/drv.c" <<'C'
#define main lain_main
#include "frontends/lain/main.c"
#undef main
#include <dirent.h>
static int open_fds(void) {
    int n = 0; DIR *d = opendir("/proc/self/fd"); struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d); return n - 1;   // less the directory stream itself
}
int main(int argc, char **argv) {
    int before = open_fds();
    int rc = lain_main(argc, argv);
    int after = open_fds();
    printf("rc=%d before=%d after=%d\n", rc, before, after);
    return rc != 0 ? 2 : (after > before ? 1 : 0);
}
C
gcc -std=c99 -O0 -w -I "$ROOT/src" -o "$D/drv" "$D/drv.c" -lm || { echo "the driver does not build"; exit 1; }
cd "$ROOT"
# A corpus program that imports std modules, compiled by relative path from the root, as run_tests
# does (an absolute path moves the module root).
"$D/drv" tests/stdlib/mem_smoke_pass.ln -o "$D/out.c"; rc=$?
[ $rc -eq 0 ] || { echo "descriptors were left open (or the compile failed): exit $rc"; exit 1; }
echo "a compile leaves no source file open"
