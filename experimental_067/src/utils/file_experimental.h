#ifndef UTILS_FILE_EXPERIMENTAL_H
#define UTILS_FILE_EXPERIMENTAL_H

#include "common/system/file.h" /* file */ /* beware for name collision with this import */
#include "common/system/memory.h" /* memory_alloc */
#include "arena.h" /* Arena */
#include "panic.h"

typedef struct
{
    file  handle;
    isize size;
    char* data;
} File;

static Arena file_arena(char* path) {
    File f;
    
    f.handle = file_open_r(path);
    panic_if(f.handle == FILE_OPEN_FAILED);

    f.size = file_size(f.handle);
    panic_if(f.size == FILE_SIZE_FAILED);

    Arena a;

    a = arena_new(memory_alloc, f.size +1); // + 1 byte to add the NUL‑terminator
    arena_push_many(&a, char, f.size + 1);
    file_read(f.handle, f.data, f.size);
    a.beg[f.size] = '\0';

    //arena_align(&a, 8);

    return a;
}

#endif /* UTILS_FILE_EXPERIMENTAL_H */