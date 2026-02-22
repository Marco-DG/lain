# Lain Compiler — Changelog Fix Critici

**Data**: 22 Febbraio 2026  
**Stato**: ✅ Tutti i test passano (positivi e negativi)

---

## Panoramica

Sono stati risolti **5 problemi critici** identificati nell'analisi del compilatore. Ogni fix rafforza le garanzie di safety del linguaggio e rimuove debito tecnico accumulato durante la fase prototipale.

---

## Fix A1 — `break` come keyword dedicata

### Problema
`break` **non era una keyword riconosciuta** dal lexer. Veniva parsata come un semplice identificatore. Funzionava per puro caso: il codice C generato conteneva la stringa `break`, che è una keyword C valida. Ma questo significava che:
- Un utente poteva scrivere `var break = 5` senza errori
- Il compilatore non aveva alcun controllo semantico su `break`
- Il comportamento poteva rompersi in modi imprevedibili

### Soluzione
Aggiunto `break` come keyword completa end-to-end, seguendo lo stesso pattern di `continue` che era già implementato correttamente.

### File modificati

| File | Modifica |
|------|----------|
| `src/token.h` | Aggiunto `TOKEN_KEYWORD_BREAK` all'enum, al riconoscimento keyword (case 5), e alle funzioni di nome/stringa |
| `src/ast.h` | Aggiunto `STMT_BREAK` all'enum `StmtKind` + costruttore `stmt_break()` |
| `src/parser/stmt.h` | Aggiunta funzione `parse_break_stmt()` + check in `parse_stmt()` |
| `src/emit/stmt.h` | Aggiunto `case STMT_BREAK:` che emette `break;` nel C generato |
| `src/ast_print.h` | Aggiunto `case STMT_BREAK:` per il debug printing dell'AST |

### Codice chiave

```diff
// token.h — Enum
+ TOKEN_KEYWORD_BREAK,

// token.h — Keyword matching
+ if (strncmp(lexeme, "break", 5) == 0)  return TOKEN_KEYWORD_BREAK;

// ast.h — Statement kind
  STMT_CONTINUE,
+ STMT_BREAK,

// parser/stmt.h — Statement parsing
+ if (parser_match(TOKEN_KEYWORD_BREAK)) {
+     return parse_break_stmt(arena, parser);
+ }

// emit/stmt.h — C code generation
+ case STMT_BREAK:
+     emit_indent(depth);
+     EMIT("break;\n");
+     break;
```

---

## Fix A6 — Globali non più forzate mutabili

### Problema
In `scope.h`, la funzione `sema_insert_global()` **hardcodava `is_mutable = true`** per ogni simbolo globale:

```c
// PRIMA (scope.h linea 59)
sym->is_mutable = true;  // ← Sempre mutabile!
```

Questo significava che anche una dichiarazione globale immutabile (senza `var`) veniva trattata come mutabile, annullando la distinzione tra variabili mutabili e immutabili a livello globale.

### Soluzione
La funzione `sema_insert_global()` ora accetta un parametro esplicito `bool is_mutable`:

```diff
// scope.h
- static void sema_insert_global(const char *raw, const char *cname, Type *ty, Decl *decl) {
+ static void sema_insert_global(const char *raw, const char *cname, Type *ty, Decl *decl, bool is_mutable) {
      ...
-     sym->is_mutable = true;
+     sym->is_mutable = is_mutable;
```

### Call sites aggiornati in `resolve.h`

| Call site | Valore `is_mutable` | Motivazione |
|-----------|---------------------|-------------|
| `DECL_VARIABLE` | `d->as.variable_decl.is_mutable` | Rispetta la dichiarazione dell'utente |
| `DECL_FUNCTION` / `DECL_PROCEDURE` | `false` | Le funzioni non sono "variabili" |
| `DECL_STRUCT` | `false` | I tipi struct non sono mutabili |
| `DECL_EXTERN_TYPE` | `false` | I tipi esterni non sono mutabili |
| `DECL_ENUM` | `false` | I tipi enum non sono mutabili |

---

## Fix A3 — Bounds checker: warning su accessi non verificabili

### Problema
In `bounds.h`, quando il bounds checker non poteva verificare staticamente un accesso a un array (perché l'indice era sconosciuto), **non emetteva alcun messaggio**:

```c
// PRIMA (bounds.h, linea 78-80)
} else {
    // Index unknown -> potentially unsafe
    // Erroring here might be too aggressive...
}
```

Questo contraddice la filosofia "Zero Runtime Checks" — un accesso potenzialmente out-of-bounds passava in silenzio.

### Soluzione
Il bounds checker ora emette un **warning** (non un errore fatale) quando non può verificare un accesso:

```diff
  } else {
-     // fprintf(stderr, "bounds warning: ...");
-     // Erroring here might be too aggressive without better range analysis.
+     fprintf(stderr, "bounds warning: index range unknown, cannot verify against length %ld\n", (long)len_range.min);
  }
+ } else {
+     // Array length unknown (dynamic slice)  
+     if (!idx.known) {
+         BOUNDS_DBG("warning: both index and array length unknown, cannot verify bounds statically");
+     }
  }
```

> **Nota di design**: Si è scelto un warning e non un errore per non rompere codice esistente. In futuro, con l'evoluzione del range analysis, molti di questi warning potranno essere risolti automaticamente. L'opzione `in` (constraint di appartenenza) è il modo idiomatico per silenziare il warning.

---

## Fix A4 — Linearità: controllo su varianti enum con campi `mov`

### Problema
In `linearity.h`, il check di linearità per le varianti ADT/enum era un **TODO**:

```c
// PRIMA (linearity.h, linea 86)
// TODO: Enums (variants with fields)
```

Se un enum aveva una variante con un campo lineare (`mov`), il compilatore **non lo rilevava** come tipo lineare. Questo significava che:
- Un `Option(mov File)` poteva essere copiato
- Un `Result(mov Connection)` poteva essere dimenticato (leaked)

### Soluzione
Aggiunta logica che itera su tutte le varianti dell'enum e controlla se almeno una ha un campo lineare:

```diff
- // TODO: Enums (variants with fields)
+ if (sym->decl->kind == DECL_ENUM) {
+     for (Variant *v = sym->decl->as.enum_decl.variants; v; v = v->next) {
+         for (DeclList *f = v->fields; f; f = f->next) {
+             if (f->decl->kind == DECL_VARIABLE) {
+                 if (sema_type_is_linear(f->decl->as.variable_decl.type)) {
+                     return true;  // Enum è lineare se QUALSIASI variante ha un campo lineare
+                 }
+             }
+         }
+     }
+ }
```

**Semantica**: Un enum è lineare se **qualsiasi** variante contiene un campo `mov`. Questo è conservativo: anche se al runtime si usa una variante senza campi lineari, l'intero tipo deve essere consumato esattamente una volta.

---

## Fix C1 — Rimozione alias `match` / `switch`

### Problema
Il lexer mappava **tre keyword diverse** allo stesso token:

```c
// PRIMA
"case"   → TOKEN_KEYWORD_CASE   // ← keyword canonica
"match"  → TOKEN_KEYWORD_CASE   // ← alias, con commento "TODO: to remove"
"switch" → TOKEN_KEYWORD_CASE   // ← alias, con FIXME
```

Esistevano anche enum `TOKEN_KEYWORD_MATCH` e `TOKEN_KEYWORD_SWITCH` che non erano mai generati (dead code).

### Soluzione
- **Rimossi** `TOKEN_KEYWORD_MATCH` e `TOKEN_KEYWORD_SWITCH` dall'enum
- **Rimossi** i mappings `"match" → CASE` e `"switch" → CASE` dal lexer
- **`case` è l'unica keyword canonica** per il pattern matching

### Test aggiornati

4 file di test usavano `match` e sono stati aggiornati a `case`:

| File | Modifica |
|------|----------|
| `tests/types/adt.ln` | `match c {` → `case c {` |
| `tests/types/enums.ln` | `match c {` → `case c {` |
| `tests/types/enum_exhaustive.ln` | `match s {` → `case s {` |
| `tests/exhaustive_fail.ln` | `match x {` → `case x {` |

> **Importante**: Se hai altro codice Lain fuori dalla cartella `tests/` che usa `match`, andrà aggiornato a `case`.

---

## Verifica

```
$ bash run_tests.sh
=== Running Positive Tests ===
[...30 test positivi...]
=== Running Negative Tests ===
[...tutti i test negativi correttamente falliti...]
All tests passed!
```

- **Build compilatore**: ✅ Zero errori
- **Test positivi**: ✅ Tutti passano (incluso `while_loop.ln` con `break`)
- **Test negativi**: ✅ Tutti correttamente rifiutati
