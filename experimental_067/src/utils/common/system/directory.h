#ifndef SYSTEM_DIRECTORY_H
#define SYSTEM_sCTORY_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
    
    from: gravity_utils.c

    TODO: REDO ALL
*/

#include "../def.h"
#include "../libc.h"
#include "../predef.h" /* OS_WINDOWS, OS_LINUX */
#include "../system.h"

#ifndef DIRECTORY_DEBUG
#define DIRECTORY_DEBUG 0
#endif
/*
bool directory_is_directory (const char *path) {
#if OS_WINDOWS
    DWORD dwAttrs = GetFileAttributesA(path);
    if (dwAttrs == INVALID_FILE_ATTRIBUTES) return false;
    if (dwAttrs & FILE_ATTRIBUTE_DIRECTORY) return true;

#elif OS_LINUX
    struct stat buf;
    
    if (lstat(path, &buf) < 0) return false;
    if (S_ISDIR(buf.st_mode)) return true;
#endif
    
    return false;
}

bool directory_create (const char *path) {
#if OS_WINDOWS
    CreateDirectoryA(path, NULL);
#elif OS_LINUX
    mode_t saved = umask(0);
    mkdir(path, 0775);
    umask(saved);
#endif
    
    return file_exists(path);
}
*/

#endif /* SYSTEM_DIRECTORY_H */