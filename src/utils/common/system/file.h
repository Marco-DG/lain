#ifndef SYSTEM_FILE_H
#define SYSTEM_FILE_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt

    adapted from:
        https://github.com/mulle-core/mulle-mmap/tree/release
        https://github.com/RealNeGate/Cuik/blob/master/common/file_map.h

    TODO: REDO ALL
*/

#include "../def.h"
#include "../libc.h"
#include "../predef.h" /* OS_WINDOWS, OS_LINUX */


#ifndef FILE_DEBUG
#define FILE_DEBUG 0
#endif

#if OS_WINDOWS
typedef HANDLE file;

#elif OS_LINUX
typedef int file;

#endif


#if OS_WINDOWS
#   define FILE_OPEN_FAILED             INVALID_HANDLE_VALUE
#   define FILE_CLOSE_FAILED            0
//#   define FILE_READ_FAILED            0
//#   define FILE_WRITE_FAILED           0
#   define FILE_MAP_FAILED              NULL

#elif OS_LINUX
#   define FILE_OPEN_FAILED             -1
#   define FILE_CLOSE_FAILED            -1
#   define FILE_READ_FAILED             -1
#   define FILE_WRITE_FAILED            -1
#   define FILE_MAP_FAILED              MAP_FAILED

#   define FILE_STDIN 0
#   define FILE_STDOUT 1

#endif

/*
struct file_map
{
    char    *data;
    int64   length;
    int64   mapped_length;
#if OS_WINDOWS
    file    mapped_handle;  On Windows, a mapped region of a file gets its own handle 
#endif
};
*/

/* If the function fails, the return value is FILE_OPEN_FAILED */
static file file_open_r(const char *path)
{
    file f;

#if OS_WINDOWS
    f = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ, /* while the file is open allow other processes to read it. */
        0,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        0
    );

#elif OS_LINUX
    f = open(path, O_RDONLY); /* flags: must include one of the following access modes: O_RDONLY, O_WRONLY, or O_RDWR, in addition status flags can be bitwise ORed. */

#endif

#if FILE_DEBUG
    assert(f != FILE_OPEN_FAILED);

#endif

    return f;
}

/* If the function fails, the return value is FILE_OPEN_FAILED */
static file file_open_rw(const char *path)
{
    file f;

#if OS_WINDOWS
    f = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, /* while the file is open allow other processes to read and write it. */
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

#elif OS_LINUX
    f = open(path, O_RDWR); /* flags: must include one of the following access modes: O_RDONLY, O_WRONLY, or O_RDWR, in addition status flags can be bitwise ORed. */

#endif

#if FILE_DEBUG
    assert(f != FILE_OPEN_FAILED);

#endif

    return f;
}

/* If the function fails, the return value is FILE_OPEN_FAILED */
static file file_open_rw_create(const char *path)
{
    file f;

#if OS_WINDOWS
    f = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, /* while the file is open allow other processes to read and write it. */
        NULL,
        OPEN_ALWAYS, /* create file if it does not exists */
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

#elif OS_LINUX
    f = open(path, O_RDWR | O_CREAT, 0644); /* flags: must include one of the following access modes: O_RDONLY, O_WRONLY, or O_RDWR, in addition status flags can be bitwise ORed. */

#endif

#if FILE_DEBUG
    assert(f != FILE_OPEN_FAILED);

#endif

    return f;
}

/* If the function fails, the return value is FILE_OPEN_FAILED */
static file file_open_rw_create_truncate(const char *path)
{
    file f;

#if OS_WINDOWS


#elif OS_LINUX
    f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644); /* flags: must include one of the following access modes: O_RDONLY, O_WRONLY, or O_RDWR, in addition status flags can be bitwise ORed. */

#endif

#if FILE_DEBUG
    assert(f != FILE_OPEN_FAILED);

#endif

    return f;
}

/* If the function fails, the return value is FILE_CLOSE_FAILED */
static void file_close(file handle)
{
#if OS_WINDOWS
    CloseHandle(handle);
#elif OS_LINUX
    close(handle);
#endif
}

/* TODO */
static inline int64 file_size(file handle)
{
#if OS_WINDOWS
    LARGE_INTEGER file_size;
    if(GetFileSizeEx(handle, &file_size) == 0) {
        return -1;
    }
	return (int64)file_size.QuadPart;

#elif OS_LINUX
    struct stat buf;
    if(fstat(handle, &buf) == -1) {
        return -1;
    }
    return buf.st_size;

#endif
}

bool file_exists (const char *path) 
{
#if OS_WINDOWS
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;

#elif OS_LINUX
    if (access(path, F_OK) == 0) return true;

#endif
    
    return false;
}

bool file_delete (const char *path)
{
#ifdef OS_WINDOWS
    return DeleteFileA(path);

#elif OS_LINUX
    if (unlink(path) == 0) return true;
    
#endif
    
    return false;
}

/* If the function fails, the return value is FILE_READ_FAILED ONLY ON LINUX */
static inline isize file_read(file handle, void *buf, usize nbytes) /* size is file_size or less ? */
{
#if OS_WINDOWS
    DWORD bytes_read; /* DWORD - uint32 */
    /* BOOL success = */ ReadFile(handle, buf, nbytes, &bytes_read, NULL);
    /*
        file_read PROC
    $LN4:
            sub     rsp, 56
            mov     r8d, DWORD PTR [rdx+8]
            lea     r9, QWORD PTR bytes_read$[rsp]
            mov     rdx, QWORD PTR [rdx]
            mov     QWORD PTR [rsp+32], 0
            call    QWORD PTR __imp_ReadFile
            mov     eax, DWORD PTR bytes_read$[rsp]
            add     rsp, 56
            ret     0
    */

#elif OS_LINUX
    ssize_t bytes_read = read(handle, buf, nbytes); /* ssize_t - int64 */ /* -1 on failure */
    /*
    file_read:
            jmp     read
    */

#endif

#if FILE_DEBUG
    assert(bytes_read > 0);

#endif

    return (isize)bytes_read;
}


static inline isize file_write(file handle, void *buf, usize nbytes) {
#if OS_WINDOWS
    DWORD bytes_written;
    BOOL success = WriteFile(handle, buf, (DWORD)nbytes, &bytes_written, NULL);

#if FILE_DEBUG
    panic_if(!success || bytes_written == 0);
#endif

#elif OS_LINUX
    ssize_t bytes_written = write(handle, buf, nbytes);

#if FILE_DEBUG
    assert(bytes_written != -1);
#endif

#endif

    return bytes_written;
}


/* Return MAP_FAILED on failure */
static void* file_map_rw(file handle, int64 length) /* lenght should be = file size; also, i think lenght gets always page aligned */
{
#if FILE_DEBUG
    assert(length > 0); /* mmap retuns FILE_MAP_FAILED if lenght is equal to zero */

#endif

    void *p;

#if OS_WINDOWS
    HANDLE mapping = CreateFileMappingA(handle, NULL, PAGE_READWRITE, 0, lenght, NULL); /* returns NULL on failure */

#if FILE_DEBUG
    panic_if(!mapping);

#endif

    p = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0, 0); /* what happens if mapping is NULL ? */
    CloseHandle(mapping); /* Close the mapping handle after mapping the view */

#elif OS_LINUX
    p = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, handle, 0);

#endif

#if FILE_DEBUG
    assert(p != FILE_MAP_FAILED);

#endif

    return p;
}

#endif /* SYSTEM_FILE_H */