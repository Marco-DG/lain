/*──────────────────────────────────────────────────────────────────╗
│ Copyright © 2024 Marco De Groskovskaja, licensed under the MIT    |
| License, see https://mit-license.org for details.                 │
╚──────────────────────────────────────────────────────────────────*/

#ifndef PANIC_H
#define PANIC_H

#include "./common/predef.h"  /* CURR_FILE, CURR_FUNC, CURR_LINE */
#include "./common/libc.h"    /* fprintf, stderr, abort */


#define PANIC_MESSAGE      "Panic:\n"                       \
                           "|         File    : %s\n"       \
                           "|         Line    : %i\n"       \
                           "|     Function    : %s\n"

#define PANIC_MSG_MESSAGE  "Panic:\n"                       \
                           "|         File    : %s\n"       \
                           "|         Line    : %i\n"       \
                           "|     Function    : %s\n"       \
                           "|       Reason    : "

#define panic()                         _panic(CURR_FILE, CURR_FUNC, CURR_LINE)
#define panic_if(expr)                  do {if((expr)) _panic(CURR_FILE, CURR_FUNC, CURR_LINE); } while(0)
#define panic_msg(fmt, ...)             _panic_msg(CURR_FILE, CURR_FUNC, CURR_LINE, fmt, ##__VA_ARGS__)
#define panic_if_msg(expr, fmt, ...)    do { if((expr)) _panic_msg(CURR_FILE, CURR_FUNC, CURR_LINE, fmt, ##__VA_ARGS__); } while(0)

static inline void _panic(const char *filename, const char *function, int line)
{
    fprintf(stderr, PANIC_MESSAGE, filename, line, function);
    abort();
}

static inline void _panic_msg(const char *filename, const char *function, int line, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    fprintf(stderr, PANIC_MSG_MESSAGE, filename, line, function);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    va_end(args);
    
    abort();
}

#endif /* PANIC_H */