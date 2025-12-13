#ifndef UTILS_FILE_H
#define UTILS_FILE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

*/

#include "common/system/file.h" /* file */ /* beware for name collision with this import */
#include "arena.h" /* Arena */

typedef struct
{
    file  handle;
    isize size;
    char* contents;
} File;

static File file_read_into_arena(Arena* arena, char* filename)
{
    File f = {0};
    
    f.handle = file_open_r(filename);
    if (f.handle == FILE_OPEN_FAILED) {
        printf("ERROR OPENING FILE");
        abort();
    }
    f.size = file_size(f.handle);
    if (f.size < 0) {
        fprintf(stderr, "Error: could not stat file '%s' (size=%zd)\n", filename, f.size);
        exit(1);
    }

    // allocate f.size + 1 bytes so we can NUL‑terminate
    char* buf = arena_push_many(arena, char, f.size + 1);
    f.contents = buf;

    // read exactly f.size bytes
    file_read(f.handle, f.contents, f.size);

    // NUL‑terminate
    buf[f.size] = '\0';

    arena_align(arena, 8);

    return f;
}

#endif /* UTILS_FILE_H */