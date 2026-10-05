 
#ifndef PARSER_DECL_H
#define PARSER_DECL_H

#include "../parser.h"

// Helper: Check if token is a comparison operator (for equation-style constraints)
static bool is_comparison_op(TokenKind kind) {
    switch (kind) {
        case TOKEN_ANGLE_BRACKET_LEFT:
        case TOKEN_ANGLE_BRACKET_RIGHT:
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
            return true;
        default:
            return false;
    }
}

// One bound of a range in a refinement (DECIDE-X, I.77): a number (negative too), a name, or a
// name's member (`src.len`; resolve decides what is meaningful). Positioned, as every refinement
// name is (M13).
static Expr *parse_refine_bound(Arena *arena, Parser *parser) {
    isize ln = parser->line, cl = parser->column;
    bool neg = false;
    if (parser_match(TOKEN_MINUS)) { neg = true; parser_advance(); }
    Expr *t = NULL;
    if (parser_match(TOKEN_NUMBER)) {
        long long v = parse_numeric_literal(parser->token.start, parser->token.length);
        parser_advance();
        t = expr_literal(arena, neg ? -v : v);
    } else if (!neg && parser_match(TOKEN_IDENTIFIER)) {
        t = expr_identifier(arena, id(arena, parser->token.length, parser->token.start));
        parser_advance();
        if (parser_match(TOKEN_DOT)) {
            parser_advance();
            parser_expect(TOKEN_IDENTIFIER, "Expected a member name after '.' in a range bound");
            Id *m = id(arena, parser->token.length, parser->token.start);
            parser_advance();
            t = expr_member(arena, t, m);
        }
    } else {
        parser_error("Expected a number or a name as a range bound after 'in'");
    }
    t->line = ln; t->col = cl;
    return t;
}

// `NAME TYPE in lo..hi` / `in lo..=hi` (DECIDE-X, I.77): membership in a range, stated as the two
// clauses it means, `NAME >= lo` and `NAME < hi` (`<= hi`), appended at **tail, so a field's go to
// the field-invariant machinery and a parameter's to the precondition, as if spelled out. Called
// at the `in`. The container form `pos usize in src`, an index test, was retired by I.78: it is
// refused here with its replacement.
static void parse_in_refinement(Arena *arena, Parser *parser, Expr *subject, ExprList ***tail) {
    parser_advance();   // 'in'
    Expr *lo = parse_refine_bound(arena, parser);
    if (!(parser_match(TOKEN_DOT_DOT) || parser_match(TOKEN_DOT_DOT_EQUAL))) {
        if (lo->kind == EXPR_IDENTIFIER) {
            Id *c = lo->as.identifier_expr.id;
            fprintf(stderr, "[E100] Error Ln %li, Col %li: `in %.*s` as an index refinement was retired: "
                    "`in` means membership in a set. Write `in 0..%.*s.len` for an index, always below "
                    "the length, or `in 0..=%.*s.len` for a cursor that may rest at the end.\n",
                    (long)lo->line, (long)lo->col, (int)c->length, c->name, (int)c->length, c->name,
                    (int)c->length, c->name);
            exit(1);
        }
        parser_error("Expected a range `lo..hi` or `lo..=hi` after 'in'");
    }
    bool inclusive = parser_match(TOKEN_DOT_DOT_EQUAL);
    parser_advance();
    Expr *hi = parse_refine_bound(arena, parser);
    Expr *c_lo = expr_binary(arena, TOKEN_ANGLE_BRACKET_RIGHT_EQUAL, subject, lo);
    c_lo->line = lo->line; c_lo->col = lo->col;
    Expr *c_hi = expr_binary(arena, inclusive ? TOKEN_ANGLE_BRACKET_LEFT_EQUAL : TOKEN_ANGLE_BRACKET_LEFT,
                             subject, hi);
    c_hi->line = hi->line; c_hi->col = hi->col;
    **tail = expr_list(arena, c_lo); *tail = &(**tail)->next;
    **tail = expr_list(arena, c_hi); *tail = &(**tail)->next;
    // More clauses may follow, joined by `and`: `x i32 in 0..100 and != 50`.
    if (parser_match(TOKEN_KEYWORD_AND)) {
        parser_advance();
        if (!is_comparison_op(parser->token.kind))
            parser_error("Expected comparison operator after 'and'");
    }
}

// Q-017 attribute parsing: [name] or [name(args)]
// Whitelist of known attribute names (lista chiusa pre-1.0):
// ★ ONE SYNTAX FOR DECLARATION METADATA (plan 7B.3, E.3). `[cold]`, `[hot]`, `[allocator]` and
// `[noreturn]` join `[private]`, `[packed]` and `[fast_math]`; the `@` forms are accepted for now
// and scheduled for removal. `@` keeps exactly one job — a compiler BUILTIN in expression
// position (`@os`, `@load`, `@splat`) — because a sigil that means one thing before a
// declaration and another inside an expression is two mechanisms sharing a character.
static bool is_known_attribute(const char *name, isize len) {
    if (len == 9 && strncmp(name, "fast_math", 9) == 0) return true;
    if (len == 7 && strncmp(name, "private",   7) == 0) return true;
    if (len == 6 && strncmp(name, "packed",    6) == 0) return true;
    if (len == 4 && strncmp(name, "cold",      4) == 0) return true;
    if (len == 3 && strncmp(name, "hot",       3) == 0) return true;
    if (len == 9 && strncmp(name, "allocator", 9) == 0) return true;
    if (len == 8 && strncmp(name, "noreturn",  8) == 0) return true;
    if (len == 7 && strncmp(name, "ordered",   7) == 0) return true;
    return false;
}

// Parse zero or more attributes [name] / [name(args)]. Returns linked list (or NULL).
// Sets is_private flag if [private] is encountered.
// Is `name` among the parsed bracket attributes? Used to set the per-declaration flags that the
// `@` forms used to set, so `[cold]` and `@cold` mean the same thing while the corpus migrates.
static bool attrs_have(Attr *a, const char *name, isize len) {
    for (; a; a = a->next)
        if (a->name && a->name->length == len && strncmp(a->name->name, name, (size_t)len) == 0)
            return true;
    return false;
}

static Attr *parse_attributes(Arena *arena, Parser *parser, bool *out_is_private) {
    Attr *head = NULL;
    Attr **tail = &head;
    *out_is_private = false;

    while (parser_match(TOKEN_L_BRACKET)) {
        parser_advance(); // consume '['

        if (!parser_match(TOKEN_IDENTIFIER)) {
            fprintf(stderr, "[E102] Error Ln %li, Col %li: expected attribute name after '['\n",
                    parser->line, parser->column);
            return head;
        }

        Id *name = id(arena, parser->token.length, parser->token.start);
        parser_advance(); // consume identifier

        // Validate against whitelist
        if (!is_known_attribute(name->name, name->length)) {
            fprintf(stderr, "[E103] Error Ln %li, Col %li: unknown attribute '%.*s' (known: private, packed, ordered, fast_math, cold, hot, allocator, noreturn)\n",
                    parser->line, parser->column, (int)name->length, name->name);
            exit(1);
        }

        // Optional args: [name(arg1, arg2, ...)]
        ExprList *args = NULL;
        if (parser_match(TOKEN_L_PAREN)) {
            parser_advance(); // consume '('
            ExprList **atail = &args;
            while (!parser_match(TOKEN_R_PAREN) && !parser_match(TOKEN_EOF)) {
                Expr *e = parse_expr(arena, parser);
                ExprList *node = arena_push_aligned(arena, ExprList);
                node->expr = e;
                node->next = NULL;
                *atail = node;
                atail = &node->next;
                if (parser_match(TOKEN_COMMA)) parser_advance();
            }
            parser_expect(TOKEN_R_PAREN, "Expected ')' after attribute arguments");
            parser_advance();
        }

        parser_expect(TOKEN_R_BRACKET, "Expected ']' to close attribute");
        parser_advance();
        parser_skip_eol();

        // Q-018: cache [private]
        if (name->length == 7 && strncmp(name->name, "private", 7) == 0) {
            *out_is_private = true;
        }

        // Append to list
        Attr *a = arena_push_aligned(arena, Attr);
        a->name = name;
        a->args = args;
        a->next = NULL;
        *tail = a;
        tail = &a->next;
    }

    return head;
}

// Helper: check if a decl has a specific attribute by name
static bool decl_has_attribute(Decl *d, const char *name, isize len) {
    if (!d || !d->attributes) return false;
    for (Attr *a = d->attributes; a; a = a->next) {
        if (a->name && a->name->length == len && strncmp(a->name->name, name, len) == 0) {
            return true;
        }
    }
    return false;
}

// entry‑point for a module
DeclList* parse_module(Arena *arena, Parser *parser);

// top‑level declarations
DeclList* parse_decl_list(Arena *arena, Parser *parser);
Decl *   parse_decl(Arena *arena, Parser *parser);
Decl *   parse_var_decl(Arena *arena, Parser *parser);
Decl *   parse_func_decl(Arena *arena, Parser *parser);
Decl *   parse_extern_func_decl(Arena *arena, Parser *parser);
Decl *   parse_extern_type_decl(Arena *arena, Parser *parser); // New
Decl *   parse_type_decl(Arena *arena, Parser *parser);
Decl *   parse_import_decl(Arena *arena, Parser *parser);
Decl *   parse_c_include_decl(Arena *arena, Parser *parser);

// helper for type fields (struct vs enum)
DeclList* parse_type_fields(Arena *arena, Parser *parser, bool *is_enum, Variant **adt_variants);

DeclList *parse_module(Arena* arena, Parser* parser) {
    //parser_skip_whitespace();
    parser_skip_eol();
    
    DeclList*  list = NULL;
    DeclList** list_tail = &list;
    bool had_top_level_error = false;

    while (!parser_match(TOKEN_EOF)) {
        Decl* decl = parse_decl(arena, parser);
        if (!decl) {
            const char *tname = token_kind_name(parser->token.kind);
            fprintf(stderr, "[E100] Error Ln %li, Col %li: Unexpected token at top level: %s\n",
                    parser->line, parser->column,
                    tname ? tname : "UNKNOWN_TOKEN");
            parser_advance(); // consume to avoid infinite loop
            had_top_level_error = true;
            continue;         // keep reporting further top-level errors...
        }

        *list_tail = decl_list(arena, decl);
        list_tail = &(*list_tail)->next;

        parser_skip_eol();
    }

    // ...but a top-level parse error is fatal: never proceed to sema/emit with a
    // partial AST and exit 0 (that silently "compiled" a file full of errors).
    if (had_top_level_error) {
        exit(1);
    }

    return list;
}


DeclList* parse_decl_list(Arena* arena, Parser* parser)
{
    DeclList*  list = NULL;
    DeclList** list_tail = &list;
    
    while (!parser_match(TOKEN_R_PAREN)) {
        //parser_advance();
        Decl* decl = parse_decl(arena, parser);

        *list_tail = decl_list(arena, decl);
        list_tail = &(*list_tail)->next;

        if (parser_match(TOKEN_COMMA)) {
            parser_advance();
        } else {
            break;
        }
    }

    return list;
}

Decl *parse_decl(Arena* arena, Parser* parser)
{
    // Q-017: parse leading attributes [name] / [name(args)]
    bool is_private = false;
    Attr *attrs = NULL;
    if (parser_match(TOKEN_L_BRACKET)) {
        attrs = parse_attributes(arena, parser, &is_private);
        parser_skip_eol();
    }

    Decl *d = NULL;

    if (parser_match(TOKEN_KEYWORD_IMPORT))
    {
        parser_advance();
        d = parse_import_decl(arena, parser);
        goto done;
    }

    if (parser_match(TOKEN_KEYWORD_C_INCLUDE))
    {
        parser_advance();
        d = parse_c_include_decl(arena, parser);
        goto done;
    }

    if (parser_match(TOKEN_KEYWORD_TYPE))
    {
        isize line = parser->line, col = parser->column;   // a type's diagnostics had Ln 0
        parser_advance();
        d = parse_type_decl(arena, parser);
        if (d && !d->line) { d->line = line; d->col = col; }
        goto done;
    }

    // DECIDE-O: `assert <constant expression>` at module scope — a claim about the program's
    // constants and LAYOUT (`assert @sizeof(Token) == 8`). Inside a function `assert(p)` is an
    // obligation the range analysis must prove; here nothing runs, so it is checked where the
    // number is known: by Lain for a plain constant, by the C compiler for @sizeof/@alignof.
    if (parser_match(TOKEN_KEYWORD_ASSERT))
    {
        isize line = parser->line, col = parser->column;
        parser_advance();
        d = arena_push_aligned(arena, Decl);
        d->kind = DECL_STATIC_ASSERT;
        d->as.static_assert_decl.cond = parse_expr(arena, parser);
        d->line = line; d->col = col;
        goto done;
    }

    // extern func …
    if (parser_match(TOKEN_KEYWORD_EXTERN)) {
        parser_advance();  // consume 'extern'
        if (parser_match(TOKEN_KEYWORD_FUNC)) {
            parser_advance();  // consume 'func'
            d = parse_extern_func_decl(arena, parser);
            // The declaration attributes were read only on the path for a function WITH a body,
            // so on an extern — the one place `[allocator]` and `[noreturn]` can be believed —
            // they parsed and were dropped. An extern's attributes are a claim about the foreign
            // function, trusted like its effect row.
            if (d) {
                d->as.function_decl.is_cold      = attrs_have(attrs, "cold", 4);
                d->as.function_decl.is_hot       = attrs_have(attrs, "hot", 3);
                d->as.function_decl.is_allocator = attrs_have(attrs, "allocator", 9);
                d->as.function_decl.is_noreturn  = attrs_have(attrs, "noreturn", 8);
            }
            goto done;
        }
        // ★ `proc` IS GONE (E.4/E.5). The token stays RESERVED so this message can exist: a
        // removed keyword that lexes as an ordinary identifier produces "expected declaration",
        // which tells a reader with old code nothing about where the feature went.
        if (parser_match(TOKEN_KEYWORD_PROC)) {
            parser_error("`proc` was removed: there is one introducer, `func`, and an effect row. "
                         "Write `extern func NAME(...) RET effects io` — and note that on an "
                         "`extern` the row NARROWS a default of `io, diverge, raises, alloc`");
        }
        if (parser_match(TOKEN_KEYWORD_TYPE)) {
            parser_advance(); // consume 'type'
            d = parse_extern_type_decl(arena, parser);
            goto done;
        }
        parser_expect(TOKEN_KEYWORD_FUNC, "Expected 'func' or 'type' after 'extern'");
        return NULL;
    }

    // Declaration metadata, from the BRACKET form (`[cold]`) with the `@` form still accepted.
    // E.3 of plan 7B: one syntax per family, and `@` keeps only its builtin job.
    bool decl_is_cold      = attrs_have(attrs, "cold", 4);
    bool decl_is_hot       = attrs_have(attrs, "hot", 3);
    bool decl_is_allocator = attrs_have(attrs, "allocator", 9);
    bool decl_is_noreturn  = attrs_have(attrs, "noreturn", 8);
    // `@io` and `@diverges` were a second spelling of `effects io` and `effects diverge`, left by
    // the incremental migration that removed `proc` (plan 7B.15); one concern spelled two ways is
    // what L3 refuses, and both are refused below as `proc` is (I.67).
    if (parser_match(TOKEN_AT)) {
        isize at_line = parser->line, at_col = parser->column;
        parser_advance(); // consume '@'
        parser_expect(TOKEN_IDENTIFIER, "Expected annotation name after '@'");
        const char *aname = parser->token.start;
        isize alen = parser->token.length;
        if      (alen == 4 && strncmp(aname, "cold",      4) == 0) decl_is_cold      = true;
        else if (alen == 3 && strncmp(aname, "hot",       3) == 0) decl_is_hot       = true;
        else if (alen == 9 && strncmp(aname, "allocator", 9) == 0) decl_is_allocator = true;
        else if (alen == 8 && strncmp(aname, "noreturn", 8) == 0)  decl_is_noreturn  = true;
        else if ((alen == 8 && strncmp(aname, "diverges", 8) == 0) ||
                 (alen == 2 && strncmp(aname, "io",       2) == 0)) {
            bool io = alen == 2;
            fprintf(stderr, "[E100] Error Ln %li, Col %li: `@%s` was removed: an effect is stated once, "
                    "in the row. Write `func NAME(...) RET effects %s`, listing every effect the "
                    "function has (silence means none).\n",
                    (long)at_line, (long)at_col, io ? "io" : "diverges", io ? "io" : "diverge");
            exit(1);
        }
        if (decl_is_cold || decl_is_hot || decl_is_allocator || decl_is_noreturn) {
            parser_advance(); // consume annotation name
            parser_skip_eol();
        }
    }

    if (parser_match(TOKEN_KEYWORD_FUNC)) {
        parser_advance();
        d = parse_func_decl(arena, parser);
        // ★ `[allocator]` becomes `__attribute__((malloc, returns_nonnull))`: the result is
        // non-null and aliases nothing. On a function with a BODY that is a claim nothing checks,
        // and a false one is undefined behaviour in the C — the corpus's own example returned
        // null. It is believed where every claim about foreign code is: on an `extern`.
        if (d && decl_is_allocator) {
            fprintf(stderr, "[E103] Error Ln %li, Col %li: [allocator] applies to an `extern` "
                    "function. It tells the C compiler the result is non-null and aliases nothing, "
                    "which a body cannot be checked to satisfy; declare the allocator `extern`.\n",
                    (long)d->line, (long)d->col);
            exit(1);
        }
        if (d) {
            d->as.function_decl.is_cold      = decl_is_cold;
            d->as.function_decl.is_hot       = decl_is_hot;
            d->as.function_decl.is_allocator = decl_is_allocator;
            d->as.function_decl.is_noreturn  = decl_is_noreturn;
        }
        goto done;
    }

    if (parser_match(TOKEN_KEYWORD_PROC)) {
        parser_error("`proc` was removed: there is one introducer, `func`, and an effect row. "
                     "Write `func NAME(...) RET effects io`, or `effects diverge` for a loop the "
                     "compiler cannot bound. Silence means no effects at all");
    }

    if (parser_match(TOKEN_KEYWORD_VAR)) {
        // No mutable global state ("no runtime globals"): a `func` reading one
        // would depend on hidden state, and it invites static-init-order hazards.
        // Top-level bindings are compile-time constants — thread runtime state
        // through arguments / a context struct instead.
        parser_error("mutable global variables are not allowed — top-level bindings "
                     "are compile-time constants. Write `NAME T = value`, and thread "
                     "runtime state explicitly.");
    }

    // Top-level compile-time constant: `NAME T = value`
    if (parser_match(TOKEN_IDENTIFIER)) {
         d = parse_var_decl(arena, parser);
         goto done;
    }

done:
    if (d) {
        d->attributes = attrs;
        d->is_private = is_private;
        // Q-002 Sprint 19: propagate [packed] to struct decl.
        if (d->kind == DECL_STRUCT && decl_has_attribute(d, "packed", 6)) {
            // covered below via the attribute walk
        }
        if (d->kind == DECL_STRUCT) {
            for (Attr *a = attrs; a; a = a->next) {
                if (a->name && a->name->length == 6
                    && strncmp(a->name->name, "packed", 6) == 0) {
                    d->as.struct_decl.is_packed = true;
                    break;
                }
            }
        }
        // [ordered] (DECIDE-U): a struct whose layout is read outside the program — a file
        // format, a wire header — keeps its declaration order, and so does every payload of a
        // sum declared [ordered]. It means nothing on anything else (a plain enumeration has no
        // payload), so it is refused there rather than accepted and ignored.
        for (Attr *a = attrs; a; a = a->next) {
            if (!a->name || a->name->length != 7 || strncmp(a->name->name, "ordered", 7) != 0) continue;
            bool carries = false;
            if (d->kind == DECL_ENUM)
                for (Variant *v = d->as.enum_decl.variants; v; v = v->next) if (v->fields) carries = true;
            if (d->kind != DECL_STRUCT && !carries) {
                fprintf(stderr, "[E103] Error Ln %li, Col %li: [ordered] applies to a struct, or "
                        "to a sum whose variants carry payloads: it keeps their fields in "
                        "declaration order.\n", (long)d->line, (long)d->col);
                exit(1);
            }
            if (d->kind == DECL_STRUCT) d->as.struct_decl.is_ordered = true;
            else d->as.enum_decl.is_ordered = true;
        }
    }
    return d;
}


// helper for type fields (struct vs enum/ADT)
DeclList* parse_type_fields(Arena *arena, struct Parser *parser, bool *is_enum, Variant **adt_variants) {
    DeclList* struct_fields = NULL;
    DeclList** struct_tail = &struct_fields;
    *adt_variants = NULL;
    Variant** variant_tail = adt_variants;
    *is_enum = false; // Default to struct, switch to enum if we see variants

    /* Skip any leading blank lines before the first field/value */
    parser_skip_eol();

    /* Loop until we hit '}' or EOF */
    while (!parser_match(TOKEN_R_BRACE) && !parser_match(TOKEN_EOF)) {
        /* Skip blank lines before each entry */
        parser_skip_eol();
        if (parser_match(TOKEN_R_BRACE)) break;

        /* Check for modifiers (Prefix syntax): mov name Type, var name Type */
        bool is_move = false;
        bool is_mut = false;
        if (parser_match(TOKEN_KEYWORD_MOV)) {
            parser_advance();
            is_move = true;
        } else if (parser_match(TOKEN_KEYWORD_VAR)) {
            parser_advance();
            is_mut = true;
        }

        /* Must start with an identifier (field name or enum value) */
        parser_expect(TOKEN_IDENTIFIER, "Expected field name or enum value");
        Id *name = id(arena, parser->token.length, parser->token.start);
        long name_line = parser->line, name_col = parser->column;   // the field's diagnostics point here
        parser_advance();

        // Lookahead to distinguish:
        // 1. Name Type -> Struct Field
        // 2. Name { ... } -> ADT Variant
        // 3. Name -> Enum Variant (if followed by separator or '}')
        
        bool is_struct_field = false;

        if (is_move || is_mut) {
             // Modifiers imply struct field
             is_struct_field = true;
        } else if (parser_match(TOKEN_L_BRACE)) {
            // Case 2: ADT Variant with fields
            // is_adt_variant = true;
        } else if (parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_KEYWORD_VAR) || parser_match(TOKEN_L_BRACKET) || parser_match(TOKEN_ASTERISK) || parser_match(TOKEN_QUESTION)) {
            // Case 1: Struct Field (followed by Type start tokens, incl. `?T` nullable)
            is_struct_field = true;
        } else {
            // Case 3: Simple Enum Variant
            // is_adt_variant = true;
        }

        if (is_struct_field) {
            if (*is_enum) {
                parser_error("Cannot mix struct fields and enum variants in the same type");
            }
            
            /* Struct field: parse its type */
            Type *field_type = parse_type(arena, parser);

            if (is_mut) field_type = type_mut(arena, field_type);
            if (is_move) field_type = type_move(arena, field_type);

            /* Create the Decl for this field */
            Decl *var_decl = decl_variable(arena, name, field_type);
            // At the field's NAME. The position after its type is the next line's first token
            // (often the struct's `}`), and every diagnostic about a field pointed there.
            var_decl->line = name_line;
            var_decl->col = name_col;

            // `in lo..hi` (a range: two clauses) or the retired `in <container>` (an index test).
            ExprList *fconstraints = NULL;
            ExprList **fctail = &fconstraints;
            Expr *field_expr = expr_identifier(arena, name);
            if (parser_match(TOKEN_KEYWORD_IN)) parse_in_refinement(arena, parser, field_expr, &fctail);

            // G5: optional field refinement constraints, e.g.
            // `type Config { pct i32 >= 0 and <= 100 }`. Same grammar as parameter
            // and alias constraints; stored on the field's variable_decl.constraints
            // and enforced at construction (see the struct-constructor check).
            if (is_comparison_op(parser->token.kind)) {
                do {
                    TokenKind op = parser->token.kind;
                    parser_advance();
                    Expr *rhs = NULL;
                    // A refinement bound may be NEGATIVE. Without this, `a i32 >= -10 and <= 10`
                    // is a PARSE ERROR, so half the range of every signed type was unreachable
                    // by a refinement and no signed-negative arithmetic could be constrained
                    // enough to prove. Folded into the literal, so every consumer downstream
                    // (the IR refinement, the alias constraints, the VRA seeding) reads an
                    // ordinary EXPR_LITERAL and needed no change.
                    bool neg = false;
                    if (parser_match(TOKEN_MINUS)) { neg = true; parser_advance(); }
                    if (parser_match(TOKEN_NUMBER)) {
                        long long value = parse_numeric_literal(parser->token.start, parser->token.length);
                        parser_advance();
                        rhs = expr_literal(arena, neg ? -value : value);
                    } else if (!neg && parser_match(TOKEN_IDENTIFIER)) {
                        Id *rid = id(arena, parser->token.length, parser->token.start);
                        isize r_line = parser->line, r_col = parser->column;
                        parser_advance();
                        rhs = expr_identifier(arena, rid);
                        rhs->line = r_line; rhs->col = r_col;
                        // `pos usize <= src.len` — another field's LENGTH, the POSITION
                        // invariant a lexer needs (EOF is pos == len). Only `.len` is meaningful
                        // on a field here; resolve decides what the name refers to.
                        if (parser_match(TOKEN_DOT)) {
                            parser_advance();
                            parser_expect(TOKEN_IDENTIFIER, "Expected `len` after '.' in a field refinement");
                            Id *mid = id(arena, parser->token.length, parser->token.start);
                            parser_advance();
                            rhs = expr_member(arena, rhs, mid);
                            rhs->line = r_line; rhs->col = r_col;
                        }
                    } else {
                        parser_error("Expected number or identifier after comparison operator");
                    }
                    { Expr *fc = expr_binary(arena, op, field_expr, rhs);
                      fc->line = parser->line; fc->col = parser->column;
                      *fctail = expr_list(arena, fc); }
                    fctail = &(*fctail)->next;
                    if (parser_match(TOKEN_KEYWORD_AND)) {
                        parser_advance();
                        if (!is_comparison_op(parser->token.kind))
                            parser_error("Expected comparison operator after 'and'");
                    } else break;
                } while (is_comparison_op(parser->token.kind));
            }
            var_decl->as.variable_decl.constraints = fconstraints;

            /* Append to struct_fields list */
            *struct_tail = decl_list(arena, var_decl);
            struct_tail = &(*struct_tail)->next;
        } else {
            // ADT/Enum Variant
            *is_enum = true;
            if (struct_fields != NULL) {
                 parser_error("Cannot mix struct fields and enum variants in the same type");
            }
            
            DeclList *variant_fields = NULL;
            
            if (parser_match(TOKEN_L_BRACE)) {
                parser_advance(); // consume '{'
                
                // Parse variant fields: Name Type, ...
                DeclList** vfields_tail = &variant_fields;
                
                while (!parser_match(TOKEN_R_BRACE) && !parser_match(TOKEN_EOF)) {
                    parser_skip_eol();
                    if (parser_match(TOKEN_R_BRACE)) break;
                    
                    parser_expect(TOKEN_IDENTIFIER, "Expected variant field name");
                    Id *fname = id(arena, parser->token.length, parser->token.start);
                    long fname_line = parser->line, fname_col = parser->column;
                    parser_advance();
                    
                    Type *ftype = parse_type(arena, parser);
                    Decl *fdecl = decl_variable(arena, fname, ftype);
                    fdecl->line = fname_line;
                    fdecl->col = fname_col;

                    // A refinement on a variant's field, as a struct field takes one (I.66):
                    // `Circle { radius i32 >= 0 and <= 1000 }` was E100 "Expected ','", while the
                    // same bound through an alias compiled. Number bounds only: a struct field may
                    // name another field because ir_field_relations asserts the relation at every
                    // write, and a variant has no such machinery, so a name here would be parsed
                    // and silently unenforced. It is E132, as an unenforceable shape is on a struct.
                    if (is_comparison_op(parser->token.kind)) {
                        ExprList *vcons = NULL, **vct = &vcons;
                        Expr *fref = expr_identifier(arena, fname);
                        do {
                            TokenKind op = parser->token.kind;
                            isize op_line = parser->line, op_col = parser->column;
                            parser_advance();
                            bool neg = false;
                            if (parser_match(TOKEN_MINUS)) { neg = true; parser_advance(); }
                            if (!parser_match(TOKEN_NUMBER)) {
                                fprintf(stderr,
                                    "[E132] Error Ln %li, Col %li: the refinement of variant field '%.*s' "
                                    "must bound it by a number (`%.*s i32 >= 0 and <= 1000`).\n"
                                    "       A field of a variant cannot be related to another field: that is a\n"
                                    "       struct invariant, checked at every write to a struct, and a variant has\n"
                                    "       none, so the relation would be accepted and never checked.\n",
                                    (long)parser->line, (long)parser->column,
                                    (int)fname->length, fname->name, (int)fname->length, fname->name);
                                exit(1);
                            }
                            long long value = parse_numeric_literal(parser->token.start, parser->token.length);
                            parser_advance();
                            Expr *fc = expr_binary(arena, op, fref, expr_literal(arena, neg ? -value : value));
                            fc->line = op_line; fc->col = op_col;
                            *vct = expr_list(arena, fc);
                            vct = &(*vct)->next;
                            if (parser_match(TOKEN_KEYWORD_AND)) {
                                parser_advance();
                                if (!is_comparison_op(parser->token.kind))
                                    parser_error("Expected comparison operator after 'and'");
                            } else break;
                        } while (is_comparison_op(parser->token.kind));
                        fdecl->as.variable_decl.constraints = vcons;
                    }
                    
                    *vfields_tail = decl_list(arena, fdecl);
                    vfields_tail = &(*vfields_tail)->next;
                    
                    if (parser_match(TOKEN_COMMA)) {
                        parser_advance();
                    } else if (parser_match(TOKEN_R_BRACE)) {
                        break;
                    } else {
                        // Optional newline separator?
                        if (parser_match(TOKEN_EOL)) {
                             parser_skip_eol();
                        } else {
                             parser_expect(TOKEN_COMMA, "Expected ',' after variant field");
                        }
                    }
                }
                
                parser_expect(TOKEN_R_BRACE, "Expected '}' after variant fields");
                parser_advance();
            }
            
            Variant *v = variant(arena, name, variant_fields);
            *variant_tail = v;
            variant_tail = &(*variant_tail)->next;
        }

        /* --- Separator handling (robust ordering) --- */

        /* 0) a closing brace ends the list: allow a final field/variant with no
              trailing separator, so single-line `{ Red, Green, Blue }` parses (the
              last item is followed directly by `}`, not by a comma or newline). */
        if (parser_match(TOKEN_R_BRACE)) break;

        /* 1) explicit comma -> eat it and continue (then skip any newlines) */
        if (parser_match(TOKEN_COMMA)) {
            parser_advance();
            parser_skip_eol();
            continue;
        }

        /* 2) explicit newline / comment separators -> consume them and continue */
        if (parser_match(TOKEN_EOL) ||
            parser_match(TOKEN_LINE_COMMENT) ||
            parser_match(TOKEN_MULTILINE_COMMENT))
        {
            parser_skip_eol();
            if (parser_match(TOKEN_R_BRACE) || parser_match(TOKEN_EOF)) break;
            continue;
        }

        /* 3) some token remains that is not a valid separator */
        parser_error("Expected ',' or newline after field or enum value");
        return struct_fields;
    }

    return struct_fields;
}


Decl* parse_type_decl(Arena* arena, Parser* parser) {
    parser_expect(TOKEN_IDENTIFIER, "Expected type name");
    Token start = parser->token;
    parser_advance();

    Token end = start;
    while (parser_match(TOKEN_DOT)) {
        parser_advance(); // .
        parser_expect(TOKEN_IDENTIFIER, "Expected identifier after dot");
        end = parser->token;
        parser_advance();
    }

    isize len = (end.start + end.length) - start.start;
    Id* name = id(arena, len, start.start);

    // Optional generic parameter header: `type Vec(T type) { … }`, `type Buf(N usize) { … }`.
    // Each entry is an ordinary `name Type` parameter (a type param has type `type`).
    DeclList *type_params = NULL;
    if (parser_match(TOKEN_L_PAREN)) {
        parser_advance(); // '('
        DeclList *tp_tail = NULL;
        if (!parser_match(TOKEN_R_PAREN)) {
            for (;;) {
                parser_expect(TOKEN_IDENTIFIER, "Expected parameter name in type header");
                Id *pn = id(arena, parser->token.length, parser->token.start);
                parser_advance();
                Type *pt = parse_type(arena, parser);
                Decl *pd = decl_variable(arena, pn, pt);
                pd->as.variable_decl.is_parameter = true;
                DeclList *node = decl_list(arena, pd);
                if (!type_params) type_params = node; else tp_tail->next = node;
                tp_tail = node;
                if (parser_match(TOKEN_COMMA)) { parser_advance(); continue; }
                break;
            }
        }
        parser_expect(TOKEN_R_PAREN, "Expected ')' after type parameters");
        parser_advance();
    }

    // Optional BACKING WIDTH of a plain enum: `type TokenKind u8 { Ident, Number, Punct }`.
    // Without it the width is the smallest that holds the variants; with it the program
    // states the width, so a record that stores the tag has a layout that does not move when
    // a variant is added. An alias needs `=` and a body needs `{`, so an identifier here is
    // unambiguous. Checked below, once the body says whether this is a plain enum.
    Token backing_tok = parser->token;
    long backing_line = 0, backing_col = 0;
    bool has_backing = false;
    if (parser_match(TOKEN_IDENTIFIER)) {
        backing_tok = parser->token;
        backing_line = parser->line;
        backing_col = parser->column;
        has_backing = true;
        parser_advance();
    }

    // If it's a type alias: type Name = Expr
    if (!has_backing && parser_match(TOKEN_EQUAL)) {
        parser_advance(); // consume '='

        // Q-002 refinement type alias detection: `type Name = int >= 0 and <= N`.
        // Snapshot parser state, try parsing a base type identifier followed by
        // a comparison operator. If matched, parse constraints; otherwise restore
        // and parse a normal expression.
        const char *snap_cur = parser->lexer ? parser->lexer->current : NULL;
        Token snap_tok = parser->token;
        long snap_line = parser->line, snap_col = parser->column;
        (void)snap_cur;

        ExprList *type_alias_constraints = NULL;
        Expr *base_type_expr = NULL;
        if (parser_match(TOKEN_IDENTIFIER)) {
            Token base_tok = parser->token;
            parser_advance();
            if (is_comparison_op(parser->token.kind)) {
                // Refinement type alias!
                Id *base_id = id(arena, base_tok.length, base_tok.start);
                base_type_expr = expr_identifier(arena, base_id);
                ExprList **ctail = &type_alias_constraints;
                do {
                    TokenKind op = parser->token.kind;
                    parser_advance();
                    Expr *rhs = NULL;
                    if (parser_match(TOKEN_NUMBER)) {
                        long long v = parse_numeric_literal(parser->token.start, parser->token.length);
                        parser_advance();
                        rhs = expr_literal(arena, v);
                    } else if (parser_match(TOKEN_IDENTIFIER)) {
                        Id *rhs_id = id(arena, parser->token.length, parser->token.start);
                        parser_advance();
                        rhs = expr_identifier(arena, rhs_id);
                    } else {
                        parser_error("Expected number or identifier in type alias refinement");
                    }
                    Expr *constraint = expr_binary(arena, op, base_type_expr, rhs);
                    constraint->line = parser->line; constraint->col = parser->column;
                    *ctail = expr_list(arena, constraint);
                    ctail = &(*ctail)->next;
                    if (parser_match(TOKEN_KEYWORD_AND)) {
                        parser_advance();
                        if (!is_comparison_op(parser->token.kind)) {
                            parser_error("Expected comparison operator after 'and'");
                        }
                    } else {
                        break;
                    }
                } while (is_comparison_op(parser->token.kind));
                Decl *d = decl_type_alias(arena, name, base_type_expr);
                d->as.type_alias_decl.constraints = type_alias_constraints;
                return d;
            } else {
                // Not a refinement — restore parser state for normal expression parse.
                parser->token = snap_tok;
                parser->line = snap_line;
                parser->column = snap_col;
                if (parser->lexer) parser->lexer->current = (char*)snap_cur;
            }
        }

        Expr *expr = parse_expr(arena, parser);
        // Optional semicolon or newline usually separates statements, but at declaration level it's handled by parse_decl
        return decl_type_alias(arena, name, expr);
    }

    // allow the '{' to be on the next line
    parser_skip_eol();

    parser_expect(TOKEN_L_BRACE, has_backing ? "Expected '{' after the enum's backing width"
                                             : "Expected '{' after type name");
    parser_advance();

    bool is_enum;
    Variant* adt_variants;
    DeclList* struct_fields = parse_type_fields(arena, parser, &is_enum, &adt_variants);

    parser_expect(TOKEN_R_BRACE, "Expected '}' at end of type definition");
    parser_advance();

    int backing_bits = 0;
    if (has_backing) {
        static const char *const widths = "u8, u16, u32 or u64";
        int bl = (int)backing_tok.length;
        const char *bs = backing_tok.start;
        if (bl == 2 && memcmp(bs, "u8", 2) == 0) backing_bits = 8;
        else if (bl == 3 && memcmp(bs, "u16", 3) == 0) backing_bits = 16;
        else if (bl == 3 && memcmp(bs, "u32", 3) == 0) backing_bits = 32;
        else if (bl == 3 && memcmp(bs, "u64", 3) == 0) backing_bits = 64;
        if (!backing_bits) {
            fprintf(stderr, "[E100] Error Ln %li, Col %li: '%.*s' cannot back enum '%.*s': "
                    "the backing width is %s.\n", backing_line, backing_col, bl, bs,
                    (int)name->length, name->name, widths);
            exit(1);
        }
        const char *why = NULL;
        Variant *payload = NULL;
        if (!is_enum) why = struct_fields ? "has fields, not variants" : "has no variants";
        else for (Variant *v = adt_variants; v; v = v->next)
            if (v->fields) { payload = v; break; }
        if (why || payload) {
            if (payload)
                fprintf(stderr, "[E100] Error Ln %li, Col %li: only a plain enum takes a backing "
                        "width (%s), and variant '%.*s' of '%.*s' carries a payload.\n",
                        backing_line, backing_col, widths, (int)payload->name->length,
                        payload->name->name, (int)name->length, name->name);
            else
                fprintf(stderr, "[E100] Error Ln %li, Col %li: only a plain enum takes a backing "
                        "width (%s), and '%.*s' %s.\n", backing_line, backing_col, widths,
                        (int)name->length, name->name, why);
            exit(1);
        }
        long long n = 0;
        for (Variant *v = adt_variants; v; v = v->next) n++;
        if (backing_bits < 64 && n > (1LL << backing_bits)) {
            fprintf(stderr, "[E100] Error Ln %li, Col %li: enum '%.*s' has %lld variants and "
                    "u%d holds %lld; the backing width is %s.\n", backing_line, backing_col,
                    (int)name->length, name->name, n, backing_bits, 1LL << backing_bits, widths);
            exit(1);
        }
    }

    if (is_enum) {
        Decl *d = decl_enum(arena, name, adt_variants);
        d->as.enum_decl.type_params = type_params;
        d->as.enum_decl.backing_bits = backing_bits;
        return d;
    } else {
        Decl *d = decl_struct(arena, name, struct_fields);
        d->as.struct_decl.type_params = type_params;
        return d;
    }
}

    // var <name> <type>
Decl *parse_var_decl(Arena* arena, Parser* parser)
{
    isize line = parser->line;
    isize col = parser->column;
    
    parser_expect(TOKEN_IDENTIFIER, "Expected variable name");
    Id *var_name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    // The type is OPTIONAL: `NAME = expr` infers it from the initializer, exactly
    // like an in-function immutable binding `x = expr`. `NAME TYPE = expr` still
    // works when an explicit type is wanted. (Without this, top-level constants
    // required a type while in-function ones did not — an inconsistency.)
    Type *var_type = parser_match(TOKEN_EQUAL) ? NULL : parse_type(arena, parser);
    Decl *d = decl_variable(arena, var_name, var_type);
    d->line = line;
    d->col = col;
    // A top-level binding is a compile-time constant, so it must have a value.
    if (parser_match(TOKEN_EQUAL)) {
        parser_advance();
        d->as.variable_decl.init = parse_expr(arena, parser);
    } else {
        parser_error("a top-level constant must have a value — write `NAME = value` or `NAME T = value`.");
    }
    return d;
}

// func <name>(<params>) <return_type> { <body> }
// F3: `effects a, b, c` — the declared effect bound. Names are the lattice's own (write,
// diverge, raises, io, alloc); `effects` with none means PURE, which is the useful end of the
// feature ("this allocates nothing" as a compile error rather than a review note).
static bool parse_effects_clause(Parser *parser, EffectSet *out) {
    if (!parser_match(TOKEN_KEYWORD_EFFECTS)) return false;
    parser_advance();
    EffectSet e = 0;
    while (parser_match(TOKEN_IDENTIFIER)) {
        const char *n = parser->token.start; int l = parser->token.length;
        // ★ `write` IS NOT SAYABLE, and that is the point of naming it here rather than
        // leaving it out of the list. Its domain is EMPTY by language design: the effect means
        // "writes mutable GLOBAL state", and a top-level `var` is [E100] — there is no shared
        // mutable state in Lain at all. So `effects write` declared a bound on something that
        // cannot happen, was always satisfied, and W130 would then advise downgrading the
        // function to `func` in the same breath.
        //
        // That is an ASSERTION OF NOTHING: a spelling that carries no information, which is
        // exactly what law L3 (one mechanism per concern) refuses. The audit said so three
        // months ago and it stayed sayable, because nothing rejects a bound that is merely
        // vacuous.
        //
        // A programmer who writes it means one of two real things, so the diagnostic names
        // both: mutating through a `var` PARAMETER (not an effect — it is in the signature,
        // and the write footprint C5 tracks is a separate and live mechanism), or doing IO.
        if      (l==5 && strncmp(n,"write",5)==0)
            parser_error("there is no `write` effect: Lain has no mutable global state "
                         "(a top-level `var` is [E100]), so the bound would always hold. "
                         "Mutation through a `var` parameter is in the signature, not the "
                         "effect row; for external side effects write `io`");
        else if (l==7 && strncmp(n,"diverge",7)==0) e |= EFFECT_DIVERGE;
        else if (l==6 && strncmp(n,"raises",6)==0)  e |= EFFECT_RAISES;
        else if (l==2 && strncmp(n,"io",2)==0)      e |= EFFECT_IO;
        else if (l==5 && strncmp(n,"alloc",5)==0)   e |= EFFECT_ALLOC;
        else parser_error("unknown effect name (expected diverge/raises/io/alloc)");
        parser_advance();
        // ★ THE COMMA IS SHARED, and the row must not swallow the enclosing list's. A row can
        // appear inside a PARAMETER LIST now that a function-POINTER type carries one:
        //
        //     func viaptr(f *func(i32) i32 effects io, x i32) i32
        //                                           ↑ this comma belongs to the parameter list
        //
        // Consuming it unconditionally made the parser read `x` as an effect name and reject the
        // program. So look past the comma: continue the row only if a KNOWN effect name follows,
        // and otherwise restore the comma for whoever owns it. The ambiguity is genuine and local,
        // and one token of lookahead settles it — which is cheaper than giving the row a second
        // spelling inside types, the thing Part 7B exists to avoid.
        if (parser_match(TOKEN_COMMA)) {
            const char *snap_cur = parser->lexer ? parser->lexer->current : NULL;
            Token snap_tok = parser->token;
            long snap_line = parser->line, snap_col = parser->column;
            parser_advance();
            bool is_effect = false;
            if (parser_match(TOKEN_IDENTIFIER)) {
                const char *m = parser->token.start; int ml = parser->token.length;
                is_effect = (ml==7 && strncmp(m,"diverge",7)==0)
                         || (ml==6 && strncmp(m,"raises",6)==0)
                         || (ml==2 && strncmp(m,"io",2)==0)
                         || (ml==5 && strncmp(m,"alloc",5)==0)
                         || (ml==5 && strncmp(m,"write",5)==0);   // rejected above, with its message
            }
            if (is_effect) continue;
            parser->token = snap_tok; parser->line = snap_line; parser->column = snap_col;
            if (parser->lexer) parser->lexer->current = (char*)snap_cur;
        }
        break;
    }
    *out = e;
    return true;
}

// One introducer, so no `is_proc`: `proc` is rejected at the dispatch site (E100).
// One term of a return constraint's right-hand side: a literal, a parameter, or a parameter's
// `.len`. `usize < a.len` is the bound a search returns ("an index into a"), and a caller can
// only USE it if the refinement can name a's length (O-7 / plan 7I, I.1).
static Expr *parse_return_constraint_term(Arena *arena, Parser *parser) {
    if (parser_match(TOKEN_NUMBER)) {
        long long value = parse_numeric_literal(parser->token.start, parser->token.length);
        parser_advance();
        return expr_literal(arena, value);
    }
    if (!parser_match(TOKEN_IDENTIFIER))
        parser_error("Expected a number, a parameter or `param.len` in a return constraint");
    Expr *t = expr_identifier(arena, id(arena, parser->token.length, parser->token.start));
    parser_advance();
    if (parser_match(TOKEN_DOT)) {
        parser_advance();
        if (!parser_match(TOKEN_IDENTIFIER) || parser->token.length != 3 ||
            strncmp(parser->token.start, "len", 3) != 0)
            parser_error("Expected `len` after `.` in a return constraint (`a.len`)");
        parser_advance();
        t = expr_member(arena, t, id(arena, 3, "len"));
    }
    return t;
}

Decl *parse_func_decl_impl(Arena* arena, Parser* parser) {
    // function name
    parser_expect(TOKEN_IDENTIFIER, "Expected function/procedure name");
    // ★ A FUNCTION DECL CARRIED NO POSITION, and nothing noticed until the effect row became
    // the central mechanism: `decl_function` never set line/col, so every diagnostic that
    // reports about a FUNCTION rather than a statement printed "Ln 0, Col 0" and showed no
    // source line — E130 (the row does not cover the body) and the decl-level E011 among them.
    // In a file with forty functions the message named the function and left finding it to the
    // reader. The name token is the right anchor: it is what the message quotes.
    long decl_line = parser->line, decl_col = parser->column;
    Id *func_name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    // parameter list
    parser_expect(TOKEN_L_PAREN, "Expected '(' after name");
    parser_advance();

    DeclList *params = NULL;
    DeclList **tail = &params;

    if (!parser_match(TOKEN_R_PAREN)) {
        do {
            // Check for modifiers first (Prefix syntax)
            bool is_mut = false;
            bool is_move = false;
            
            if (parser_match(TOKEN_KEYWORD_VAR)) {
                parser_advance();
                is_mut = true;
            } else if (parser_match(TOKEN_KEYWORD_MOV)) {
                parser_advance();
                is_move = true;
            }

            // check for parameter name OR destructuring
            Decl *pdecl = NULL;
            
            if (parser_match(TOKEN_L_BRACE)) {
                // Destructuring: {a, b} Type
                isize d_line = parser->line, d_col = parser->column;   // for its diagnostics
                parser_advance(); // consume '{'
                
                IdList* names = NULL;
                IdList** names_tail = &names;

                do {
                    parser_expect(TOKEN_IDENTIFIER, "Expected field name in destructuring");
                    Id* field_name = id(arena, parser->token.length, parser->token.start);
                    parser_advance();

                    *names_tail = id_list(arena, field_name);
                    names_tail = &(*names_tail)->next;

                    if (parser_match(TOKEN_COMMA)) {
                        parser_advance();
                    } else {
                        break;
                    }
                } while (true);

                parser_expect(TOKEN_R_BRACE, "Expected '}' after destructuring list");
                parser_advance();

                // parameter type
                Type *ptype = parse_type(arena, parser);
                
                if (is_mut) ptype = type_mut(arena, ptype);
                if (is_move) ptype = type_move(arena, ptype);
                
                pdecl = decl_destruct(arena, names, ptype);
                pdecl->line = d_line; pdecl->col = d_col;
                
                *tail = decl_list(arena, pdecl);
                tail  = &(*tail)->next;
            } else {
                // Normal parameter: [var|mov] name Type
                isize param_line = parser->line;
                isize param_col = parser->column;
                
                parser_expect(TOKEN_IDENTIFIER, "Expected parameter name");
                Id *pname = id(arena, parser->token.length, parser->token.start);
                parser_advance();

                // parameter type
                Type *ptype = parse_type(arena, parser);
                
                if (is_mut) ptype = type_mut(arena, ptype);
                if (is_move) ptype = type_move(arena, ptype);

                pdecl = decl_variable(arena, pname, ptype);
                pdecl->line = param_line;
                pdecl->col = param_col;
                pdecl->as.variable_decl.is_parameter = true;
            // ... (constraints check continues below) ...

                // `i usize in lo..hi` (a range: two clauses) or the retired `i usize in arr`.
                ExprList *constraints = NULL;
                ExprList **ctail = &constraints;
                Expr *param_expr = expr_identifier(arena, pname);   // the LHS of every clause
                if (parser_match(TOKEN_KEYWORD_IN)) parse_in_refinement(arena, parser, param_expr, &ctail);
                
                // Parse equation-style constraints: param int != 0, param int >= 0 and <= 100
                if (is_comparison_op(parser->token.kind)) {
                    do {
                        TokenKind op = parser->token.kind;
                        long cst_line = parser->line, cst_col = parser->column;  // the clause's anchor
                        parser_advance();  // consume operator
                        
                        // Parse the RHS (literal or identifier). A NEGATIVE bound is folded
                        // into the literal — see the struct-field site above for why.
                        Expr *rhs = NULL;
                        bool pneg = false;
                        if (parser_match(TOKEN_MINUS)) { pneg = true; parser_advance(); }
                        if (parser_match(TOKEN_NUMBER)) {
                            long long value = parse_numeric_literal(parser->token.start, parser->token.length);
                            parser_advance();
                            rhs = expr_literal(arena, pneg ? -value : value);
                        } else if (!pneg && parser_match(TOKEN_IDENTIFIER)) {
                            Id *rhs_id = id(arena, parser->token.length, parser->token.start);
                            // A refinement's names carry their position: an error about one
                            // (`end usize <= h.zz`) said Ln 0 while the same expression in the
                            // body was placed (M13).
                            isize r_line = parser->line, r_col = parser->column;
                            parser_advance();
                            rhs = expr_identifier(arena, rhs_id);
                            rhs->line = r_line; rhs->col = r_col;
                            // G8: allow `ident.member` (e.g. `i usize < a.len`) as a
                            // constraint RHS — the dependent bound against a length.
                            if (parser_match(TOKEN_DOT)) {
                                parser_advance();  // consume '.'
                                parser_expect(TOKEN_IDENTIFIER, "Expected identifier after '.'");
                                Id *member = id(arena, parser->token.length, parser->token.start);
                                parser_advance();
                                rhs = expr_member(arena, rhs, member);
                                rhs->line = r_line; rhs->col = r_col;
                            }
                        } else {
                            parser_error("Expected number or identifier after comparison operator");
                        }

                        // ── C6: THE BOUND MAY BE AN EXPRESSION ──────────────────────────
                        // `n usize <= cap - 1`, `i usize < a.len - 1`. Until now the RHS was a
                        // single term, so a bound one-off from another quantity — which is what
                        // a window, a capacity or a last-index IS — could not be stated at all.
                        //
                        // ADDITIVE ONLY, and that is the domain's boundary rather than a
                        // shortcut: the octagon holds `±x ±y <= c`, so `n <= cap - 1` becomes
                        // the relation `n - cap <= -1` it already keeps exactly. `n <= 2 * k`
                        // is outside it, and accepting a bound the engine must immediately
                        // approximate would buy syntax and lose proofs.
                        //
                        // ⚠ The arithmetic is lowered CHECKED, so `cap - 1` on an unbounded
                        // usize raises the underflow obligation rather than wrapping to
                        // SIZE_MAX and making the precondition vacuous. A caller writes
                        // `cap usize >= 1` beside it and both prove. Fail-closed: a bound that
                        // cannot be evaluated is refused, never silently widened.
                        while (parser_match(TOKEN_PLUS) || parser_match(TOKEN_MINUS)) {
                            TokenKind aop = parser->token.kind;
                            parser_advance();
                            Expr *term = NULL;
                            if (parser_match(TOKEN_NUMBER)) {
                                long long v = parse_numeric_literal(parser->token.start, parser->token.length);
                                parser_advance();
                                term = expr_literal(arena, v);
                            } else if (parser_match(TOKEN_IDENTIFIER)) {
                                Id *tid = id(arena, parser->token.length, parser->token.start);
                                isize t_line = parser->line, t_col = parser->column;
                                parser_advance();
                                term = expr_identifier(arena, tid);
                                term->line = t_line; term->col = t_col;
                                if (parser_match(TOKEN_DOT)) {
                                    parser_advance();
                                    parser_expect(TOKEN_IDENTIFIER, "Expected identifier after '.'");
                                    Id *m = id(arena, parser->token.length, parser->token.start);
                                    parser_advance();
                                    term = expr_member(arena, term, m);
                                    term->line = t_line; term->col = t_col;
                                }
                            } else {
                                parser_error("Expected number or identifier after '+' or '-' in a refinement bound");
                            }
                            rhs = expr_binary(arena, aop, rhs, term);
                        }

                        // Create binary constraint expression. ★ WITH A POSITION: a refinement
                        // clause is an ordinary expression to every later pass, so a diagnostic about
                        // it (`a i32[4] >= 0` — `>=` is not defined on an array) reported
                        // "Ln 0, Col 0" and showed no source line. Same omission as `decl_function`'s
                        // missing line/col, one construct over; the constraint's own token is the
                        // anchor a reader needs.
                        Expr *constraint = expr_binary(arena, op, param_expr, rhs);
                        constraint->line = cst_line; constraint->col = cst_col;
                        *ctail = expr_list(arena, constraint);
                        ctail = &(*ctail)->next;
                        
                        // Check for 'and' to chain more constraints
                        if (parser_match(TOKEN_KEYWORD_AND)) {
                            parser_advance();
                            if (!is_comparison_op(parser->token.kind)) {
                                parser_error("Expected comparison operator after 'and'");
                            }
                        } else {
                            break;
                        }
                    } while (is_comparison_op(parser->token.kind));
                }
                pdecl->as.variable_decl.constraints = constraints;

                *tail = decl_list(arena, pdecl);
                tail  = &(*tail)->next;
            }

            if (parser_match(TOKEN_COMMA)) {
                parser_advance();
            } else {
                break;
            }
        } while (true);
    }

    parser_expect(TOKEN_R_PAREN, "Expected ')' after parameters");
    parser_advance();

    // --- return type ---
    Type *ret_type = NULL;
    if (parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_KEYWORD_VAR) || parser_match(TOKEN_ASTERISK) || parser_match(TOKEN_QUESTION)) {
        ret_type = parse_type(arena, parser);
    }

    // --- F1: `in <param>` — which parameter the RETURNED reference borrows. Reuses the `in`
    // keyword the language already has for the same KIND of idea (`pos usize in text` says an
    // index is valid in a container; `var i32 in a` says a reference borrows a parameter).
    Id *ret_borrow_of = NULL;
    if (ret_type && parser_match(TOKEN_KEYWORD_IN)) {
        parser_advance();
        parser_expect(TOKEN_IDENTIFIER, "Expected a parameter name after `in`");
        ret_borrow_of = id(arena, parser->token.length, parser->token.start);
        parser_advance();
    }

    // --- return type constraints (equation-style): int >= 0, int >= lo and <= hi ---
    ExprList *return_constraints = NULL;
    if (ret_type && is_comparison_op(parser->token.kind)) {
        ExprList **rc_tail = &return_constraints;
        
        // Create 'result' identifier for LHS of constraint
        Id *result_id = id(arena, 6, "result");
        Expr *result_expr = expr_identifier(arena, result_id);
        
        do {
            TokenKind op = parser->token.kind;
            parser_advance();  // consume operator
            
            // Parse the RHS: terms joined by `+`/`-` (`a.len`, `a.len - 1`, `n + 1`).
            Expr *rhs = parse_return_constraint_term(arena, parser);
            while (parser_match(TOKEN_PLUS) || parser_match(TOKEN_MINUS)) {
                TokenKind aop = parser->token.kind;
                parser_advance();
                rhs = expr_binary(arena, aop, rhs, parse_return_constraint_term(arena, parser));
            }
            
            // Create binary constraint expression: result op rhs
            Expr *constraint = expr_binary(arena, op, result_expr, rhs);
            constraint->line = parser->line; constraint->col = parser->column;
            *rc_tail = expr_list(arena, constraint);
            rc_tail = &(*rc_tail)->next;
            
            // Check for 'and' to chain more constraints
            if (parser_match(TOKEN_KEYWORD_AND)) {
                parser_advance();
                if (!is_comparison_op(parser->token.kind)) {
                    parser_error("Expected comparison operator after 'and' in return constraint");
                }
            } else {
                break;
            }
        } while (is_comparison_op(parser->token.kind));
    }

    // NOTE: pre/post keywords removed - use equation-style constraints instead:
    // - Parameters: func div(a int, b int != 0) int
    // - Return: func abs(x int) int >= 0

    // ── THE CLAUSE ORDER IS rettype -> refinement -> effects -> decreasing (plan 7B.3) ──
    // This call used to sit BEFORE the return-constraint block, which made
    // `func f(m usize) usize <= m effects raises` a parse error: the refinement ran first in the
    // grammar but second in the natural reading, so the two clauses could not both appear. Every
    // corpus function with a refined return type was therefore unable to declare an effect at
    // all — invisible while `proc` granted IO, and immediately fatal once the row had to.
    EffectSet eff_bound = 0; bool eff_declared = parse_effects_clause(parser, &eff_bound);

    // Optional `decreasing <measure>` clause — permits recursion in a `func`
    // (which is otherwise total and recursion-free): each self-call must strictly
    // decrease this well-founded measure. Same keyword as the loop measure.
    Expr *decreasing_measure = NULL;
    if (parser_match(TOKEN_KEYWORD_DECREASING)) {
        parser_advance();
        decreasing_measure = parse_expr(arena, parser);
    }

    // function body
    parser_expect(TOKEN_L_BRACE, "Expected '{' after signature");
    parser_advance();

    StmtList *body = parse_stmt_list(arena, parser);

    parser_expect(TOKEN_R_BRACE, "Expected '}' at end of body");
    parser_advance();

    Decl *d = decl_function(arena, func_name, params, ret_type, body, false, false);
    d->line = decl_line; d->col = decl_col;
    d->as.function_decl.return_constraints = return_constraints;
    d->as.function_decl.ret_borrow_of = ret_borrow_of;
    d->as.function_decl.effects_declared = eff_declared;
    d->as.function_decl.effects_bound    = eff_bound;
    d->as.function_decl.decreasing_measure = decreasing_measure;
    d->as.function_decl.decreasing_written = (decreasing_measure != NULL);   // set at PARSE time
    return d;
}

Decl *parse_func_decl(Arena* arena, Parser* parser) {
    return parse_func_decl_impl(arena, parser);
}



// extern func <name>(<params>) <return> ;
Decl *parse_extern_func_decl_impl(Arena *arena, Parser *parser) {
    long decl_line = parser->line, decl_col = parser->column;   // see parse_func_decl_impl
    // name
    parser_expect(TOKEN_IDENTIFIER, "Expected function/procedure name");
    Id *func_name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    // parameters
    parser_expect(TOKEN_L_PAREN, "Expected '(' after name");
    parser_advance();

    DeclList *params = NULL;
    DeclList **tail = &params;

    bool is_variadic = false;

    if (!parser_match(TOKEN_R_PAREN)) {
        do {
            // Check for varargs "..."
            if (parser_match(TOKEN_ELLIPSIS)) {
                 parser_advance();
                 is_variadic = true;
                 break;
            }
            // Backward compatibility: "..." manually written as ".. ."
            if (parser_match(TOKEN_DOT_DOT) && lexer_peek(parser->lexer).kind == TOKEN_DOT) {
                 // consume ".." then "."
                 parser_advance(); 
                 parser_advance();
                 is_variadic = true;
                 break;
            }

            // parameter name
            isize param_line = parser->line;
            isize param_col = parser->column;
            parser_expect(TOKEN_IDENTIFIER, "Expected parameter name");
            Id *pname = id(arena, parser->token.length, parser->token.start);
            parser_advance();

            // parameter type
            Type *ptype = parse_type(arena, parser);

            Decl *pdecl = decl_variable(arena, pname, ptype);
            pdecl->line = param_line;
            pdecl->col = param_col;
            pdecl->as.variable_decl.is_parameter = true;
            *tail = decl_list(arena, pdecl);
            tail = &(*tail)->next;

            if (parser_match(TOKEN_COMMA)) {
                parser_advance();
            } else {
                break;
            }
        } while (true);
    }
    
    parser_expect(TOKEN_R_PAREN, "Expected ')' after parameters");
    parser_advance();

    // return type
    Type *ret_type = NULL;
    if (parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_KEYWORD_VAR) || parser_match(TOKEN_ASTERISK)) {
        ret_type = parse_type(arena, parser);
    }

    // F1: `in <param>` on an extern is where the annotation actually earns its keep — there
    // is no body, so nothing can infer WHICH parameter the result borrows, and the fallback is
    // "all of them".
    Id *ext_borrow_of = NULL;
    if (ret_type && parser_match(TOKEN_KEYWORD_IN)) {
        parser_advance();
        parser_expect(TOKEN_IDENTIFIER, "Expected a parameter name after `in`");
        ext_borrow_of = id(arena, parser->token.length, parser->token.start);
        parser_advance();
    }

    EffectSet ext_eff = 0; bool ext_eff_declared = parse_effects_clause(parser, &ext_eff);

    // require end-of-decl (newline or semicolon)
    parser_expect_eol("Expected ';' or newline after extern decl");
    parser_advance();

    // NULL body signals extern
    Decl *ed = decl_function(arena, func_name, params, ret_type, /*body=*/NULL, true, is_variadic);
    ed->line = decl_line; ed->col = decl_col;
    ed->as.function_decl.ret_borrow_of = ext_borrow_of;
    ed->as.function_decl.effects_declared = ext_eff_declared;
    ed->as.function_decl.effects_bound = ext_eff;
    return ed;
}

Decl *parse_extern_func_decl(Arena *arena, Parser *parser) {
    return parse_extern_func_decl_impl(arena, parser);
}

Decl *parse_extern_type_decl(Arena *arena, Parser *parser) {
    // extern type Name;
    parser_expect(TOKEN_IDENTIFIER, "Expected type name after 'extern type'");
    Id *name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    parser_expect_eol("Expected ';' or newline after extern type decl");
    parser_advance();

    return decl_extern_type(arena, name);
}


// parser.h (below your other parse_* declarations)
Decl *parse_import_decl(Arena* arena, Parser* parser) {
    // Start with first identifier
    parser_expect(TOKEN_IDENTIFIER, "Expected module name after import");
    isize imp_line = parser->line, imp_col = parser->column;   // diagnostics about the import
    Token start = parser->token;
    parser_advance();

    // Loop to collect dotted path; a `.{ … }` ends it with a selective list.
    Token end = start;
    IdList *selected = NULL;
    IdList **sel_tail = &selected;
    bool has_selective = false;
    while (parser_match(TOKEN_DOT)) {
        parser_advance();  // consume dot
        if (parser_match(TOKEN_L_BRACE)) {
            // Selective import: `import foo.bar.{ a, b }` — a, b come in unqualified.
            has_selective = true;
            parser_advance();  // consume '{'
            while (!parser_match(TOKEN_R_BRACE)) {
                parser_expect(TOKEN_IDENTIFIER, "Expected a name in the import list");
                Id *nm = id(arena, parser->token.length, parser->token.start);
                parser_advance();
                *sel_tail = id_list(arena, nm);
                sel_tail = &(*sel_tail)->next;
                if (parser_match(TOKEN_COMMA)) parser_advance();
                else break;
            }
            parser_expect(TOKEN_R_BRACE, "Expected '}' to close the import list");
            parser_advance();
            break;
        }
        parser_expect(TOKEN_IDENTIFIER, "Expected identifier after '.'");
        end = parser->token;
        parser_advance();
    }

    // Splice tokens together to form "foo.bar" from start to end
    size_t len = (end.start + end.length) - start.start;
    Id* mod = id(arena, len, start.start);

    Decl *d = decl_import(arena, mod);
    d->line = imp_line; d->col = imp_col;   // it had none: an error about an import said Ln 0
    if (has_selective) d->as.import_decl.selected = selected;
    // Optional alias: `import foo.bar as baz` → qualified access via `baz.`
    if (parser_match(TOKEN_KEYWORD_AS)) {
        parser_advance();
        parser_expect(TOKEN_IDENTIFIER, "Expected an alias identifier after 'as'");
        d->as.import_decl.alias = id(arena, parser->token.length, parser->token.start);
        parser_advance();
    }
    return d;
}

Decl *parse_c_include_decl(Arena *arena, Parser *parser) {
    parser_expect(TOKEN_STRING_LITERAL, "Expected string literal after c_include");
    
    // Strip quotes to get the content
    // e.g. "stdio.h" -> stdio.h
    //      "<stdio.h>" -> <stdio.h>
    
    isize len = parser->token.length;
    const char* raw = parser->token.start;
    
    char* path = arena_push_many(arena, char, len - 1); // len-2 chars + 1 null terminator
    // Skip first quote, copy len-2 chars
    if (len >= 2) {
        memcpy(path, raw + 1, len - 2);
        path[len - 2] = '\0';
    } else {
        path[0] = '\0'; // Should not happen for valid string literal
    }

    parser_advance(); // consume string literal
    
    return decl_c_include(arena, path); 
}

#endif // PARSER_DECL_H