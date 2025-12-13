#ifndef PARSER_H
#define PARSER_H

/*
include/
└── parser/
    ├── parser.h         - umbrella header
    ├── core.h           - Parser struct, advance/expect, macros
    ├── decl.h           - parse_decl*, parse_module, parse_type_decl, import
    ├── stmt.h           - parse_stmt_list, parse_stmt*, for/use, var/assign
    ├── expr.h           - parse_expr*, binary, unary, primary
    └── type.h           - parse_type, slice/array suffixes
*/

#include "lexer.h"
#include "ast.h"

/* keep this import order */
#include "parser/core.h"
#include "parser/type.h"
#include "parser/expr.h"
#include "parser/stmt.h"
#include "parser/decl.h"

#endif // PARSER_H
