/*──────────────────────────────────────────────────────────────────╗
│ Copyright © 2024 Marco De Groskovskaja, licensed under the MIT    |
| License, see https://mit-license.org for details.                 │
╚──────────────────────────────────────────────────────────────────*/

#ifndef DEBUG_H
#define DEBUG_H

#include "./common/predef.h"  /* CURR_FILE, CURR_FUNC, CURR_LINE */
#include "./common/libc.h"    /* fprintf, stderr */
#include <stdarg.h>           /* va_list, va_start, va_end */

#define DEBUG_MESSAGE      "Debug:\n"                       \
                           "|         File    : %s\n"       \
                           "|         Line    : %i\n"       \
                           "|     Function    : %s\n"

#define DEBUG_MSG_MESSAGE  "Debug:\n"                       \
                           "|         File    : %s\n"       \
                           "|         Line    : %i\n"       \
                           "|     Function    : %s\n"       \
                           "|       Message   : "

#ifdef DEBUG_OFF

#define debug()                         ((void)0)
#define debug_if(expr)                  ((void)0)
#define debug_msg(fmt, ...)             ((void)0)
#define debug_if_msg(expr, fmt, ...)    ((void)0)

#else

#define debug()                         _debug(CURR_FILE, CURR_FUNC, CURR_LINE)
#define debug_if(expr)                  do { if ((expr)) _debug(CURR_FILE, CURR_FUNC, CURR_LINE); } while(0)
#define debug_msg(fmt, ...)             _debug_msg(CURR_FILE, CURR_FUNC, CURR_LINE, fmt, ##__VA_ARGS__)
#define debug_if_msg(expr, fmt, ...)    do { if ((expr)) _debug_msg(CURR_FILE, CURR_FUNC, CURR_LINE, fmt, ##__VA_ARGS__); } while(0)

static inline void _debug(const char *filename, const char *function, int line)
{
    fprintf(stderr, DEBUG_MESSAGE, filename, line, function);
}

static inline void _debug_msg(const char *filename, const char *function, int line, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    fprintf(stderr, DEBUG_MSG_MESSAGE, filename, line, function);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    va_end(args);
}

#endif /* DEBUG_OFF */

#endif /* DEBUG_H */