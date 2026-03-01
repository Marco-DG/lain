#ifndef ARGS_H
#define ARGS_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "utils/common/libc.h"

typedef struct
{
    char*   filename;
    char*   output_file;  // -o flag, defaults to "out.c"
    bool    dump_ast;
} Args;

static void _args_help(void)
{
    printf("### Lain Compiler ###\n");
    printf("Usage: <path_to_file_to_compile> [options]\n");
    printf("Options:\n");
    printf("  --dump-ast      Print the AST after parsing\n");
    printf("  -o <file>       Set output C file (default: out.c)\n");
}

static Args args_parse(int argc, char** argv)
{
    if (argc <= 0) { exit(EXIT_SUCCESS); }
    if (argc == 1) { _args_help(); exit(EXIT_SUCCESS); }
    
    Args args = {0};
    args.output_file = "out.c";  // default
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-ast") == 0) {
            args.dump_ast = true;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            args.output_file = argv[++i];
        } else {
            args.filename = argv[i];
        }
    }
    
    if (!args.filename) {
        printf("Error: No input file specified.\n");
        exit(1);
    }
    
    return args;
}

#endif /* ARGS_H */