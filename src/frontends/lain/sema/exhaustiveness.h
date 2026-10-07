#ifndef SEMA_EXHAUSTIVENESS_H
#define SEMA_EXHAUSTIVENESS_H

/*
 * Exhaustiveness Checking for Lain Match Statements
 * 
 * Ensures that match statements cover all possible cases.
 * Supports:
 * - Enum types: all variants must be covered (or have else:)
 * - Bool types: true and false must be covered (or have else:)
 * - Integer types: must have else: (infinite domain)
 */

#include "../ast.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

// Debug flag
#ifndef SEMA_EXHAUSTIVENESS_DEBUG
#define SEMA_EXHAUSTIVENESS_DEBUG 0
#endif

#if SEMA_EXHAUSTIVENESS_DEBUG
#define EXHAUST_DBG(fmt, ...) fprintf(stderr, "[exhaust] " fmt "\n", ##__VA_ARGS__)
#else
#define EXHAUST_DBG(fmt, ...) do {} while(0)
#endif

// Forward declaration for symbol lookup (from resolve.h)
extern DeclList *sema_decls;

/*───────────────────────────────────────────────────────────────────╗
│ Helper Functions                                                    │
╚───────────────────────────────────────────────────────────────────*/

// Check if a match statement has an else case (catch-all)
static bool match_has_else_case(StmtMatchCase *cases) {
    for (StmtMatchCase *c = cases; c; c = c->next) {
        if (c->patterns == NULL) {
            EXHAUST_DBG("found else case");
            return true;
        }
    }
    return false;
}

// Find enum declaration by type name
static Decl *find_enum_decl(Type *vtype) {
    if (!vtype || vtype->kind != TYPE_SIMPLE || !vtype->base_type) {
        return NULL;
    }
    
    const char *type_name = vtype->base_type->name;
    int type_len = vtype->base_type->length;
    
    // ★ EXACT NAMES FIRST, A SUFFIX ONLY IF UNIQUE (I.105). One loop tried both, so with
    // `type Shade` declared before `type Dark_Shade`, a `Dark_Shade` scrutinee ("…_Shade") was
    // checked against Shade: a `case` covering Shade's variants was accepted as exhaustive, and
    // Dark_Shade's other variant fell off its end.
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        if (!dl->decl || dl->decl->kind != DECL_ENUM) continue;
        Id *enum_name = dl->decl->as.enum_decl.type_name;
        if (enum_name && enum_name->length == type_len &&
            strncmp(enum_name->name, type_name, type_len) == 0) return dl->decl;
    }
    // A suffix match handles mangled names like module_Enum.
    Decl *hit = NULL; int hits = 0;
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        if (!dl->decl || dl->decl->kind != DECL_ENUM) continue;
        Id *enum_name = dl->decl->as.enum_decl.type_name;
        if (!enum_name || type_len <= enum_name->length + 1) continue;
        const char *suffix_start = type_name + (type_len - enum_name->length);
        if (*(suffix_start - 1) == '_' &&
            strncmp(suffix_start, enum_name->name, enum_name->length) == 0) { hit = dl->decl; hits++; }
    }
    if (hits > 1) {
        fprintf(stderr, "internal error: the type name '%.*s' matches %d enums by name; please "
                "report it.\n", type_len, type_name, hits);
        exit(1);
    }
    return hit;
}

// ★ A VARIANT PATTERN NAMES A VARIANT OF THE SCRUTINEE'S OWN ENUM. Nothing checked it, and
// lowering finds a variant by its bare name: `Purple:` on a `Color` (no such variant) branched
// into its arm for EVERY value, and `Light.Green:` on a `Color` tested `Color.Green`. A
// qualifier is compared only when it names a non-generic enum: an instance of a generic one
// has a mangled name (`Option_ptr_u8`), and the corpus writes no qualified pattern at all.
// The name test is lowering's own (ir_variant_index with `suffix`): a pattern that resolved
// to a constructor carries its C name, `prog_Option_Some`, so `_Some` at the end names `Some`.
static void sema_check_variant_patterns(Type *vtype, ExprList *patterns) {
    Decl *ed = find_enum_decl(vtype);
    if (!ed) return;
    Id *en = ed->as.enum_decl.type_name;
    for (ExprList *p = patterns; p; p = p->next) {
        Expr *pe = p->expr;
        Expr *pv = (pe && pe->kind == EXPR_CALL) ? pe->as.call_expr.callee : pe;
        if (!pv) continue;
        Id *vn = NULL; Decl *qd = NULL; bool mangled = false;
        // A bare name carries its variant (I.105); a qualified one is the variant's own name. Only
        // an identifier with neither, a constructor's C name, is matched by suffix.
        if (pv->kind == EXPR_IDENTIFIER) {
            vn = pv->as.identifier_expr.variant ? pv->as.identifier_expr.variant->name : pv->as.identifier_expr.id;
            mangled = !pv->as.identifier_expr.variant;
        } else if (pv->kind == EXPR_MEMBER) {
            vn = pv->as.member_expr.member;
            Expr *t = pv->as.member_expr.target;
            if (t && (t->kind == EXPR_IDENTIFIER || t->kind == EXPR_TYPE) && t->decl &&
                t->decl->kind == DECL_ENUM && !t->decl->as.enum_decl.type_params) qd = t->decl;
        } else continue;                          // a literal or a range: not a variant pattern
        if (!vn) continue;
        bool found = false;
        if (!qd || qd == ed)
            for (Variant *v = ed->as.enum_decl.variants; v && !found; v = v->next) {
                if (!v->name) continue;
                Id *w = v->name;
                found = (w->length == vn->length && strncmp(w->name, vn->name, (size_t)vn->length) == 0)
                     || (mangled && vn->length > w->length && vn->name[vn->length - w->length - 1] == '_' &&
                         strncmp(vn->name + (vn->length - w->length), w->name, (size_t)w->length) == 0);
            }
        if (found) continue;
        Id *qn = qd ? qd->as.enum_decl.type_name : NULL;
        fprintf(stderr, "[E106] Error Ln %li, Col %li: `%.*s%s%.*s` is not a variant of '%.*s'.\n",
                pe->line, pe->col, qn ? (int)qn->length : 0, qn ? qn->name : "", qn ? "." : "",
                (int)vn->length, vn->name, en ? (int)en->length : 1, en ? en->name : "?");
        diagnostic_show_line(pe->line, pe->col);
        exit(1);
    }
}

// Check if a pattern matches an enum variant by name
// Handles mangled names like "module_Type_Variant" matching variant "Variant"
static bool pattern_matches_variant(Expr *pattern, Id *variant) {
    if (!pattern || !variant) return false;
    
    // Pattern should be an identifier or a call (constructor)
    Id *pat_id = NULL;
    
    // A bare variant carries the variant the resolver bound it to (I.105): compare THAT name, not
    // the mangled one, whose suffix `_Red` also ends `Dark_Red`.
    Expr *bare = pattern->kind == EXPR_IDENTIFIER ? pattern
               : (pattern->kind == EXPR_CALL && pattern->as.call_expr.callee &&
                  pattern->as.call_expr.callee->kind == EXPR_IDENTIFIER) ? pattern->as.call_expr.callee : NULL;
    if (bare && bare->as.identifier_expr.variant) {
        Id *vn = bare->as.identifier_expr.variant->name;
        return vn && vn->length == variant->length && strncmp(vn->name, variant->name, (size_t)variant->length) == 0;
    }
    bool mangled = false;   // only an identifier with no recorded variant is a C name to suffix-match
    if (pattern->kind == EXPR_IDENTIFIER) {
        pat_id = pattern->as.identifier_expr.id; mangled = true;
    } else if (pattern->kind == EXPR_MEMBER) {
        // ★ A QUALIFIED variant, `Color.Red`: spec 15 allows it beside the bare `Red`, and it was
        // never counted, so a `case` covering every variant by the qualified spelling was refused
        // as non-exhaustive (E014) unless it added an `else:`. That the qualifier is the
        // scrutinee's own type is E106's rule, checked where the pattern is typed.
        pat_id = pattern->as.member_expr.member;
    } else if (pattern->kind == EXPR_CALL) {
        // Constructor pattern: Variant(...)
        // The callee should be the variant name
        Expr *callee = pattern->as.call_expr.callee;
        if (callee->kind == EXPR_IDENTIFIER) {
            pat_id = callee->as.identifier_expr.id; mangled = true;
        } else if (callee->kind == EXPR_MEMBER) {
             // Handle Shape.Circle(...)
             pat_id = callee->as.member_expr.member;
        }
    }
    
    if (!pat_id || !pat_id->name || !variant->name) return false;
    
    // First try exact match
    if (pat_id->length == variant->length &&
        strncmp(pat_id->name, variant->name, variant->length) == 0) {
        return true;
    }
    
    // Try suffix match: pattern ends with "_Variant"
    // e.g., "tests_enums_Color_Red" ends with "_Red". Never for a QUALIFIED pattern, whose member
    // is the variant's own name (I.105: `Shade.Dark_Red:` was counted as covering Red).
    if (mangled && pat_id->length > variant->length + 1) {
        const char *suffix_start = pat_id->name + (pat_id->length - variant->length);
        // Check if character before suffix is '_'
        if (*(suffix_start - 1) == '_' &&
            strncmp(suffix_start, variant->name, variant->length) == 0) {
            return true;
        }
    }
    
    return false;
}

// Check if all enum variants are covered
static bool match_check_enum_exhaustiveness(Decl *enum_decl, StmtMatchCase *cases) {
    if (!enum_decl) return false;
    
    Variant *variants = enum_decl->as.enum_decl.variants;
    
    // For each variant, check if there's a matching case
    for (Variant *v = variants; v; v = v->next) {
        if (!v->name) continue;
        
        EXHAUST_DBG("checking variant '%.*s'", (int)v->name->length, v->name->name);
        
        bool variant_covered = false;
        for (StmtMatchCase *c = cases; c; c = c->next) {
            if (c->patterns == NULL) {
                // else case covers everything
                variant_covered = true;
                break;
            }
            
            for (ExprList *p = c->patterns; p; p = p->next) {
                if (pattern_matches_variant(p->expr, v->name)) {
                    variant_covered = true;
                    break;
                }
            }
            if (variant_covered) break;
        }
        
        if (!variant_covered) {
            EXHAUST_DBG("enum variant '%.*s' not covered", 
                       (int)v->name->length, v->name->name);
            return false;
        }
    }
    
    EXHAUST_DBG("all enum variants covered");
    return true;
}

// Check if a match on a boolean covers both true and false
static bool match_check_bool_exhaustiveness(StmtMatchCase *cases) {
    bool has_true = false;
    bool has_false = false;
    
    for (StmtMatchCase *c = cases; c; c = c->next) {
        if (c->patterns == NULL) {
            return true;  // else covers everything
        }
        for (ExprList *p = c->patterns; p; p = p->next) {
            if (p->expr->kind == EXPR_LITERAL) {
                if (p->expr->as.literal_expr.value != 0) {
                    has_true = true;
                } else {
                    has_false = true;
                }
            }
        }
    }
    
    return has_true && has_false;
}

/*───────────────────────────────────────────────────────────────────╗
│ Main Exhaustiveness Check                                          │
╚───────────────────────────────────────────────────────────────────*/

// Main exhaustiveness check function
// Returns true if the match is exhaustive, false otherwise
static bool sema_check_match_exhaustive(Stmt *match_stmt) {
    if (!match_stmt || match_stmt->kind != STMT_MATCH) {
        return true;  // Not a match, nothing to check
    }
    
    Expr *value = match_stmt->as.match_stmt.value;
    StmtMatchCase *cases = match_stmt->as.match_stmt.cases;
    
    if (!cases) {
        EXHAUST_DBG("match has no cases!");
        return false;  // Empty match is not exhaustive
    }
    
    // Quick check: if there's an else case, always exhaustive
    if (match_has_else_case(cases)) {
        EXHAUST_DBG("match is exhaustive (has else)");
        return true;
    }
    
    // Check based on the type of the matched value
    Type *vtype = value ? value->type : NULL;
    
    if (vtype && vtype->kind == TYPE_SIMPLE && vtype->base_type) {
        const char *type_name = vtype->base_type->name;
        int type_len = vtype->base_type->length;
        
        // Check for bool
        if (type_len == 4 && strncmp(type_name, "bool", 4) == 0) {
            if (match_check_bool_exhaustiveness(cases)) {
                EXHAUST_DBG("match on bool is exhaustive");
                return true;
            }
        }
        
        // Check for enum type
        Decl *enum_decl = find_enum_decl(vtype);
        if (enum_decl) {
            if (match_check_enum_exhaustiveness(enum_decl, cases)) {
                return true;
            }
            // Fall through to error - enum not fully covered
        }
    }
    
    // For integer/other types without else, not exhaustive
    EXHAUST_DBG("match is NOT exhaustive (no else, not complete coverage)");
    return false;
}

static void type_describe(Type *t, char *buf, size_t cap);   // sema/typecheck.h

// ★ WHAT A NON-EXHAUSTIVE `case` LEAVES OUT, for its message. The statement form said only
// "non-exhaustive match", with no line, and the expression form gave a line but no reason; the
// checker knows exactly which variants are uncovered. `arms` holds each arm's pattern list.
// Writes the uncovered variants (`C.B, C.D`) or the missing bool value(s); leaves buf empty when
// the arms cannot cover every value (an integer, a string), which only an `else:` can.
static void match_describe_uncovered(Type *vtype, ExprList **arms, int narms, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!vtype || vtype->kind != TYPE_SIMPLE || !vtype->base_type) return;
    if (vtype->base_type->length == 4 && strncmp(vtype->base_type->name, "bool", 4) == 0) {
        bool has_t = false, has_f = false;
        for (int a = 0; a < narms; a++)
            for (ExprList *p = arms[a]; p; p = p->next)
                if (p->expr && p->expr->kind == EXPR_LITERAL) {
                    if (p->expr->as.literal_expr.value) has_t = true; else has_f = true;
                }
        snprintf(buf, cap, "%s", !has_t && !has_f ? "`true` or `false`" : !has_t ? "`true`" : "`false`");
        return;
    }
    Decl *ed = find_enum_decl(vtype);
    if (!ed || ed->kind != DECL_ENUM) return;
    Id *en = ed->as.enum_decl.type_name;
    size_t o = 0; int n = 0;
    for (Variant *v = ed->as.enum_decl.variants; v && o < cap; v = v->next) {
        if (!v->name) continue;
        bool covered = false;
        for (int a = 0; a < narms && !covered; a++)
            for (ExprList *p = arms[a]; p && !covered; p = p->next)
                if (pattern_matches_variant(p->expr, v->name)) covered = true;
        if (covered) continue;
        // A union (`Small | NotFound`) is written by its parts, not by the enum it lowers to:
        // its payload has no pattern, only an `else:` reaches it, and a marker is written bare.
        // The message named `__U_Small_NotFound.__payload` (I.129).
        if (ed->as.enum_decl.is_union && v == ed->as.enum_decl.variants)
            o += (size_t)snprintf(buf + o, cap - o, "%sthe value (an `else:` arm)", n++ ? ", " : "");
        else if (ed->as.enum_decl.is_union)
            o += (size_t)snprintf(buf + o, cap - o, "%s%.*s", n++ ? ", " : "",
                                  (int)v->name->length, v->name->name);
        else
            o += (size_t)snprintf(buf + o, cap - o, "%s%.*s.%.*s", n++ ? ", " : "",
                                  en ? (int)en->length : 0, en ? en->name : "",
                                  (int)v->name->length, v->name->name);
    }
}

// One message for both forms of `case`.
static void sema_report_uncovered(isize line, isize col, Expr *value, ExprList **arms, int narms) {
    char missing[256], tb[128];
    Type *vt = value ? value->type : NULL;
    type_describe(vt, tb, sizeof tb);
    match_describe_uncovered(vt, arms, narms, missing, sizeof missing);
    if (narms == 0)
        fprintf(stderr, "[E014] Error Ln %li, Col %li: this `case` has no arms.\n", (long)line, (long)col);
    else if (missing[0])
        fprintf(stderr, "[E014] Error Ln %li, Col %li: this `case` on '%s' has no arm for %s. "
                "Add one, or an `else:`.\n", (long)line, (long)col, tb, missing);
    else
        fprintf(stderr, "[E014] Error Ln %li, Col %li: a `case` on '%s' needs an `else:` arm: its "
                "patterns cannot name every value.\n", (long)line, (long)col, tb);
    diagnostic_show_line(line, col);
}

// ★ EVERY VALUE EXACTLY ONCE (E014, spec 15). A `case` must cover every value of its scrutinee,
// and name none twice: an exact duplicate pattern (`1:` then `1:`, or `A.B, A.B`) was accepted,
// and its later occurrence was dead: f(1) took the first arm and nothing said so (Documentation's
// grammar audit). The comparison is exact and fails closed: two patterns it cannot compare
// (a constructor pattern with bindings, anything not below) count as different, never as the same.
// An arm SUBSUMED by earlier ones without equalling one (`1..5:` after `1..10:`) is the general
// case, not checked here.
static bool case_pattern_same(Expr *a, Expr *b) {
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
        case EXPR_LITERAL:    return a->as.literal_expr.value == b->as.literal_expr.value &&
                                     a->as.literal_expr.is_bool == b->as.literal_expr.is_bool;
        case EXPR_CHAR:       return a->as.char_expr.value == b->as.char_expr.value;
        case EXPR_STRING:     return a->as.string_expr.length == b->as.string_expr.length &&
                                     memcmp(a->as.string_expr.value, b->as.string_expr.value,
                                            (size_t)a->as.string_expr.length) == 0;
        case EXPR_IDENTIFIER: return expr_struct_equal(a, b);
        case EXPR_MEMBER: {   // `Color.Red`: its target is the RESOLVED type, which expr_struct_equal
            Expr *ta = a->as.member_expr.target, *tb = b->as.member_expr.target;   // cannot compare
            if (ta && tb && ta->kind == EXPR_TYPE && tb->kind == EXPR_TYPE) {
                Type *x = ta->as.type_expr.type_value, *y = tb->as.type_expr.type_value;
                bool same = x == y || (x && y && x->kind == TYPE_SIMPLE && y->kind == TYPE_SIMPLE &&
                                       x->base_type && y->base_type &&
                                       x->base_type->length == y->base_type->length &&
                                       memcmp(x->base_type->name, y->base_type->name, (size_t)x->base_type->length) == 0);
                Id *ma = a->as.member_expr.member, *mb = b->as.member_expr.member;
                return same && ma && mb && ma->length == mb->length &&
                       memcmp(ma->name, mb->name, (size_t)ma->length) == 0;
            }
            return expr_struct_equal(a, b);
        }
        case EXPR_UNARY:      return a->as.unary_expr.op == b->as.unary_expr.op &&
                                     case_pattern_same(a->as.unary_expr.right, b->as.unary_expr.right);
        case EXPR_RANGE:      return a->as.range_expr.inclusive == b->as.range_expr.inclusive &&
                                     case_pattern_same(a->as.range_expr.start, b->as.range_expr.start) &&
                                     case_pattern_same(a->as.range_expr.end, b->as.range_expr.end);
        default:              return false;
    }
}
static void sema_check_arms_once(ExprList **arms, int narms, isize line, isize col) {
    for (int i = 0; i < narms; i++)
        for (ExprList *p = arms[i]; p; p = p->next)
            for (int j = 0; j <= i; j++)
                for (ExprList *q = arms[j]; q && !(j == i && q == p); q = q->next) {
                    if (!case_pattern_same(p->expr, q->expr)) continue;
                    isize pl = p->expr->line ? p->expr->line : line, pc = p->expr->line ? p->expr->col : col;
                    isize ql = q->expr->line ? q->expr->line : line;
                    fprintf(stderr, "[E014] Error Ln %li, Col %li: this pattern is already covered by the "
                            "arm at Ln %li: a `case` names each value once, and the later one is never "
                            "chosen.\n", (long)pl, (long)pc, (long)ql);
                    diagnostic_show_line(pl, pc);
                    exit(1);
                }
}

// Report non-exhaustive match error
static void sema_report_nonexhaustive_match(Stmt *match_stmt) {
    ExprList *arms[256]; int n = 0;
    for (StmtMatchCase *c = match_stmt->as.match_stmt.cases; c && n < 256; c = c->next)
        arms[n++] = c->patterns;
    sema_report_uncovered(match_stmt->line, match_stmt->col, match_stmt->as.match_stmt.value, arms, n);
}

static void sema_report_nonexhaustive_match_expr(Expr *match_expr) {
    ExprList *arms[256]; int n = 0;
    for (ExprMatchCase *c = match_expr->as.match_expr.cases; c && n < 256; c = c->next)
        arms[n++] = c->patterns;
    sema_report_uncovered(match_expr->line, match_expr->col, match_expr->as.match_expr.value, arms, n);
}

/*───────────────────────────────────────────────────────────────────╗
│ Expression Match Exhaustiveness Check                              │
╚───────────────────────────────────────────────────────────────────*/

static bool match_has_else_case_expr(ExprMatchCase *cases) {
    for (ExprMatchCase *c = cases; c; c = c->next) {
        if (c->patterns == NULL) return true;
    }
    return false;
}

static bool match_check_bool_exhaustiveness_expr(ExprMatchCase *cases) {
    bool has_true = false, has_false = false;
    for (ExprMatchCase *c = cases; c; c = c->next) {
        if (c->patterns == NULL) return true;
        for (ExprList *p = c->patterns; p; p = p->next) {
            if (p->expr->kind == EXPR_LITERAL) {
                if (p->expr->as.literal_expr.value != 0) has_true = true;
                else has_false = true;
            }
        }
    }
    return has_true && has_false;
}

static bool match_check_enum_exhaustiveness_expr(Decl *enum_decl, ExprMatchCase *cases) {
    if (!enum_decl || enum_decl->kind != DECL_ENUM) return false;
    for (Variant *v = enum_decl->as.enum_decl.variants; v; v = v->next) {
        bool variant_covered = false;
        for (ExprMatchCase *c = cases; c; c = c->next) {
            if (c->patterns == NULL) {
                variant_covered = true; break;
            }
            for (ExprList *p = c->patterns; p; p = p->next) {
                if (pattern_matches_variant(p->expr, v->name)) {
                    variant_covered = true; break;
                }
            }
            if (variant_covered) break;
        }
        if (!variant_covered) return false;
    }
    return true;
}

static bool sema_check_expr_match_exhaustive(Expr *match_expr) {
    if (!match_expr || match_expr->kind != EXPR_MATCH) return true;
    ExprMatchCase *cases = match_expr->as.match_expr.cases;
    if (!cases) return false;
    if (match_has_else_case_expr(cases)) return true;
    Type *vtype = match_expr->as.match_expr.value ? match_expr->as.match_expr.value->type : NULL;
    if (vtype && vtype->kind == TYPE_SIMPLE && vtype->base_type) {
        const char *tname = vtype->base_type->name;
        int tlen = vtype->base_type->length;
        if (tlen == 4 && strncmp(tname, "bool", 4) == 0) {
            if (match_check_bool_exhaustiveness_expr(cases)) return true;
        }
        Decl *enum_decl = find_enum_decl(vtype);
        if (enum_decl) {
            if (match_check_enum_exhaustiveness_expr(enum_decl, cases)) return true;
        }
    }
    return false;
}

#endif /* SEMA_EXHAUSTIVENESS_H */

