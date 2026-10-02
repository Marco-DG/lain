#ifndef UTILS_FILE_H
#define UTILS_FILE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

*/

#include <stdlib.h> /* malloc */
#include "common/system/file.h" /* file */ /* beware for name collision with this import */

typedef struct
{
    file  handle;
    isize size;
    char* contents;
} File;

// Reads a whole source file into its OWN allocation of exactly size + 1 bytes, NUL-terminated.
// Tokens point into it for the whole compile, so it is never freed. It was a slice of an arena that
// held every source file, and then a read past the NUL landed in the arena's next bytes (another
// file, or zeroes), where nothing could see it: the lexer did that for every unterminated literal
// at the end of a file (I.101), and an ASan build of lain reported nothing. Now a read past the
// end is a read past an allocation, which ASan reports.
static File file_read_source(char* filename)
{
    File f = {0};

    f.handle = file_open_r(filename);
    if (f.handle == FILE_OPEN_FAILED) {
        // Leave f.contents = NULL so the caller reports a useful diagnostic.
        // (Fixed C.1: previously abort() here hid the actual path failure.)
        return f;
    }
    f.size = file_size(f.handle);
    if (f.size < 0) {
        fprintf(stderr, "lain: could not stat file '%s' (size=%zd)\n", filename, f.size);
        exit(1);
    }

    // allocate f.size + 1 bytes so we can NUL‑terminate
    char* buf = malloc((size_t)f.size + 1);
    if (!buf) {
        fprintf(stderr, "lain: out of memory reading '%s'\n", filename);
        exit(1);
    }
    f.contents = buf;

    // read exactly f.size bytes
    file_read(f.handle, f.contents, f.size);

    // NUL‑terminate
    buf[f.size] = '\0';

    return f;
}

#endif /* UTILS_FILE_H */