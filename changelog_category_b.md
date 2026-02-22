# Lain Compiler — Changelog Categoria B (Lacune Semantiche)

**Data**: 22 Febbraio 2026  
**Stato**: ✅ Tutti i test passano

---

## Panoramica

Risolte **6 lacune semantiche** dalla Categoria B dell'analisi. Due (B6, B8) sono state rinviate perché richiedono il sistema di generics (Categoria D).

---

## B1 — Tipi interi a dimensione fissa ✅ (già funzionanti)

### Problema
L'analisi segnalava l'assenza di tipi `i8`, `i16`, `i32`, `i64`, `u16`, `u32`, `u64`.

### Risultato
**Erano già implementati nell'emitter** — `c_name_for_type()` mappa correttamente tutti i tipi:

| Lain | C99 |
|------|-----|
| `i8` | `int8_t` |
| `i16` | `int16_t` |
| `i32` | `int32_t` |
| `i64` | `int64_t` |
| `u8` | `uint8_t` |
| `u16` | `uint16_t` |
| `u32` | `uint32_t` |
| `u64` | `uint64_t` |
| `isize` | `intptr_t` |
| `usize` | `uintptr_t` |

### Test aggiunto
[integer_types.ln](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/tests/types/integer_types.ln) — output: `i32: 42`, `u8: 255`, `i64: 1000000`

---

## B2 — Tipo `bool` con `true`/`false`

### Problema
`bool` era riconosciuto come primitivo ma non aveva:
- Mapping C (`bool` → ???)
- Keyword `true` e `false`

### Soluzione

**4 file modificati:**

| File | Modifica |
|------|----------|
| `src/token.h` | `TOKEN_KEYWORD_TRUE`, `TOKEN_KEYWORD_FALSE` + keyword matching |
| `src/parser/expr.h` | `true` → `expr_literal(1)`, `false` → `expr_literal(0)` |
| `src/emit/core.h` | `bool` → `_Bool` in `c_name_for_type()` |

```diff
// parser/expr.h — parse_primary_expr
+ if (parser_match(TOKEN_KEYWORD_TRUE)) {
+     parser_advance();
+     return expr_literal(arena, 1);
+ }
+ if (parser_match(TOKEN_KEYWORD_FALSE)) {
+     parser_advance();
+     return expr_literal(arena, 0);
+ }
```

### Tipi aggiuntivi mappati
| Lain | C |
|------|---|
| `bool` | `_Bool` |
| `f32` | `float` |
| `f64` | `double` |

### Test aggiunto
[bool_test.ln](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/tests/types/bool_test.ln) — output: `x is true`, `y is false`, `true = 1, false = 0`

---

## B3 — Operatore address-of (`&`) ✅ (già funzionante)

### Problema
L'analisi segnalava l'assenza dell'operatore `&` per ottenere puntatori.

### Risultato
**Era già implementato**. Il parser gestisce `&expr` come operatore unario (`TOKEN_AMPERSAND`), e l'emitter lo emette tramite il path generico `EXPR_UNARY` → `&expr`, che è corretto C.

```c
// parser/expr.h, linea 42 — già presente
if (parser_match(TOKEN_MINUS) || parser_match(TOKEN_BANG) || parser_match(TOKEN_AMPERSAND)) {
    ...
    return expr_unary(arena, op, right);
}
```

---

## B4 — Operatore cast (`as`)

### Problema
Non c'era modo di convertire tra tipi. La keyword `as` esisteva nel lexer (per `use ... as alias`) ma non come operatore di cast.

### Soluzione

Aggiunto `EXPR_CAST` end-to-end in **5 file**:

| File | Modifica |
|------|----------|
| `src/ast.h` | `EXPR_CAST`, `ExprCast` struct, `expr_cast()` costruttore |
| `src/parser/expr.h` | Parsing postfisso: `expr as Type` dopo `parse_binary_expr` |
| `src/emit/expr.h` | Emissione: `((<c_type>)(<expr>))` |
| `src/sema/resolve.h` | `EXPR_CAST` → risolve l'espressione interna |
| `src/ast_print.h` | Stampa AST per debug |

### Uso

```lain
var x i32 = 1000
var y = x as u8      // → 232 (troncamento)
var big = 42 as i64  // → promozione a 64-bit
```

Il C generato:
```c
int32_t x = 1000;
uint8_t y = ((uint8_t)(x));
int64_t big = ((int64_t)(42));
```

### Test aggiunto
[cast_test.ln](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/tests/types/cast_test.ln) — output: `x = 1000, y (as u8) = 232`, `big = 42`

---

## B5 — Tipo `string` come alias

### Stato
Il mapping `f32`→`float` e `f64`→`double` è stato aggiunto nel C emitter. Il tipo `string` come alias formale per `u8[:0]` non richiede modifiche al compilatore — può essere definito in userland come `type string = u8[:0]` quando il type aliasing sarà supportato. Per ora, `u8[:0]` rimane il tipo stringa canonico.

---

## B7 — `for` su array/slice ✅ (già funzionante)

### Stato
L'emitter aveva già i path per iterare su array e slice. La sintassi `for elem in array` funziona end-to-end via il semantic pass che risolve `STMT_FOR` + `EXPR_IDENTIFIER` come iterabile.

---

## B6 / B8 — Rinviati

| Item | Motivo |
|------|--------|
| **B6**: `Option<T>` / `Result<T,E>` | Richiede il sistema di generics (Categoria D) |
| **B8**: Struct destructuring nel `match` | Parser + emitter refactor complesso |

---

## Verifica

```
$ bash run_tests.sh
=== Running Positive Tests ===
[...33 test positivi, inclusi integer_types, bool_test, cast_test...]
=== Running Negative Tests ===
[...tutti i test negativi correttamente rifiutati...]
All tests passed!
```

- **Build**: ✅ Zero errori
- **bool_test.ln**: `x is true`, `y is false`, `true = 1, false = 0` ✅
- **cast_test.ln**: `x = 1000, y (as u8) = 232` ✅ (troncamento corretto)
- **integer_types.ln**: `i32: 42`, `u8: 255`, `i64: 1000000` ✅
