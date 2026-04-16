# Analysis 16 — Documentation Update, Standard Library Types, Error Codes, Persistent Borrowed Match

**Data**: 6 marzo 2026
**LOC compilatore**: ~14.500 (C99 core)
**Test**: 124 file, 48 eseguiti dal runner, tutti passano
**README**: 2151 righe (da ~1989)
**Moduli standard**: 6 (c, io, fs, math, option, result) + 1 placeholder (string)

---

## 1. Panoramica dei cambiamenti

Questa sessione ha completato tre aree di lavoro:

| Area | Cambiamento | Impatto |
|------|-------------|---------|
| Libreria standard | `std/option.ln`, `std/result.ln` con generici comptime | Sblocca error handling idiomatico |
| Borrow checker | Borrowed match persistente (non temporaneo) | Impedisce mutazione del scrutinee in `case &` |
| Diagnostica | 15 codici errore [E001]-[E015] su ~35 siti | Errori searchabili e categorizzati |
| Documentazione | README.md aggiornato (+162 righe) | Documentazione completa delle nuove feature |

---

## 2. Standard Library: `std/option.ln` e `std/result.ln`

### 2.1 Option(T)

```lain
// std/option.ln
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}
```

Uso:
```lain
import std.option
type OptInt = Option(int)

var a = OptInt.Some(42)
var b = OptInt.None
```

Sfrutta il sistema di generici comptime (Phase B) per creare tipi ADT parametrizzati. Il compilatore monomorphizza `Option(int)` in un tipo concreto con varianti `Some { value int }` e `None`.

### 2.2 Result(T, E)

```lain
// std/result.ln
func Result(comptime T type, comptime E type) type {
    return type {
        Ok { value T }
        Err { error E }
    }
}
```

Uso:
```lain
import std.result
type MyResult = Result(int, int)

var r = MyResult.Ok(15)
case r {
    Ok(v):  return v
    Err(e): return e
}
```

### 2.3 Limitazione scoperta: expression-form match con ADT

Durante il testing, l'expression-form match (`var x = case a { Some(v): v ... }`) ha rivelato un problema di codegen: la variabile pattern-bound `v` risulta undeclared nel C generato. Il workaround è usare statement-form match:

```lain
// NON FUNZIONA (codegen issue):
var x = case a { Some(v): v, None: 0 }

// FUNZIONA:
var x = 0
case a { Some(v): x = v, None: x = 0 }
```

Questo è un bug dell'emitter, non del sistema di tipi.

### 2.4 std/string.ln — Placeholder

`std/string.ln` è stato ridotto a un placeholder con `import std.c` e un TODO. Il problema fondamentale: Lain emette `*u8` come `uint8_t*`, ma le funzioni C string (`strlen`, `strcmp`) aspettano `char*`. La fix richiede un meccanismo di type-mapping nel emitter (es. un tipo `char` nativo o un attributo di casting).

---

## 3. Persistent Borrowed Match

### 3.1 Problema

La versione precedente (Sprint 5) registrava un borrow condiviso **temporaneo** per `case &x`. I borrow temporanei vengono cancellati da `borrow_clear_temporaries()` alla fine dello statement di valutazione del scrutinee — *prima* che il corpo del match venga eseguito. Questo significava che il body del match poteva mutare il scrutinee senza errore:

```lain
case &x {
    42: x = 99    // Questo passava silenziosamente!
}
```

### 3.2 Soluzione: borrow persistente con cleanup esplicito

Il borrow è ora registrato come **persistente** (`is_temporary = false`), e viene rimosso esplicitamente alla fine del blocco match tramite `borrow_remove_entry()`.

#### Nuova funzione: `borrow_remove_entry()` (`src/sema/region.h`)

```c
static void borrow_remove_entry(BorrowTable *t, BorrowEntry *target) {
    if (!t || !target) return;
    BorrowEntry **curr = &t->head;
    while (*curr) {
        if (*curr == target) {
            *curr = (*curr)->next;
            return;
        }
        curr = &(*curr)->next;
    }
}
```

#### Modifica handler STMT_MATCH (`src/sema/linearity.h`)

```c
case STMT_MATCH: {
    BorrowEntry *borrowed_match_entry = NULL;
    if (s->as.match_stmt.is_borrowed) {
        // Register PERSISTENT shared borrow (is_temporary = false)
        borrow_register(tbl->arena, tbl->borrows,
                        owner_id, owner_id, MODE_SHARED, owner_region, false);
        borrowed_match_entry = tbl->borrows->head;
    }
    // ... process match arms ...
    // Cleanup: remove the persistent borrow
    if (borrowed_match_entry && tbl->borrows) {
        borrow_remove_entry(tbl->borrows, borrowed_match_entry);
    }
}
```

Lo stesso pattern è applicato all'handler EXPR_MATCH.

### 3.3 Risultato

```lain
case &x {
    42: x = 99    // [E004] Error: cannot mutate 'x' because it is borrowed by 'x'
}
```

Il test `tests/core/match_borrow_pass.ln` è stato aggiornato per non mutare il scrutinee (modifica una variabile separata `result`). Il test `tests/types/match_borrow_mut_fail.ln` verifica che la mutazione è rifiutata.

---

## 4. Codici Errore [E001]-[E015]

### 4.1 Schema

Ogni messaggio di errore è ora prefissato con un codice in formato `[EXXX]`:

| Codice | Categoria | File principali |
|--------|-----------|-----------------|
| [E001] | Use-after-move | linearity.h |
| [E002] | Unconsumed linear value | linearity.h |
| [E003] | Double move | linearity.h |
| [E004] | Borrow conflict | region.h, linearity.h |
| [E005] | Move of borrowed value | linearity.h |
| [E006] | Move in loop | linearity.h |
| [E007] | Branch inconsistency | linearity.h |
| [E008] | Linear field error | linearity.h |
| [E009] | Immutability violation | resolve.h |
| [E010] | Dangling reference / outlive | region.h, resolve.h |
| [E011] | Purity violation | resolve.h |
| [E012] | Type error | typecheck.h, resolve.h |
| [E013] | Redeclaration | resolve.h |
| [E014] | Exhaustiveness | typecheck.h |
| [E015] | Division by zero | typecheck.h |

### 4.2 Implementazione

Modifica meccanica: ogni `fprintf(stderr, "Error Ln ...")` è stato prefissato con il codice appropriato. Esempio:

```c
// Prima:
fprintf(stderr, "Error Ln %li, Col %li: cannot use '%s': value was moved\n", ...);

// Dopo:
fprintf(stderr, "[E001] Error Ln %li, Col %li: cannot use '%s': value was moved\n", ...);
```

~35 siti modificati su 5 file. Zero rischio di regressione (i test negativi verificano il codice di uscita, non il formato dello stderr).

---

## 5. Aggiornamento README.md

### 5.1 Sezioni aggiunte/modificate

| Sezione | Modifica |
|---------|----------|
| §5.3 Borrowing Rules | Aggiunta sottosezione "Two-Phase Borrows" con esempio UFCS |
| §7.5 Pattern Matching | Aggiunta sottosezione "Non-Consuming Match (`case &expr`)" con 3 esempi |
| §10.2 Standard Library | Aggiunte `std/math.ln`, `std/option.ln`, `std/result.ln`, `std/string.ln` (placeholder) |
| §13.1 Error Codes | Nuova sezione con tabella di tutti i 15 codici errore ed esempio di output |
| §14.4 Test Framework | Aggiornati conteggi test (124 file, 48 eseguiti) |
| §15.2 Error Model | Riscritto per riflettere che generici comptime sono implementati |
| §21 Compile-Time Generics | Riscritto con esempi aggiornati (ADT `type { }`, Result multi-parametro) |
| Appendix A | `comptime` aggiornato da "Reserved" a "Implemented" |
| Appendix C Grammar | `case_stmt` aggiornato con `["&"]` opzionale |

### 5.2 Delta

- Righe: 1989 → 2151 (+162)
- Nuove sezioni: 2 (§13.1 Error Codes, Two-Phase Borrows)
- Sezioni riscritte: 2 (§15.2, §21)

---

## 6. Metriche

| Metrica | analysis15 | Attuale | Delta |
|---------|-----------|---------|-------|
| LOC compilatore | ~14.500 | ~14.500 | +0 |
| File test | 96 | 124 | +28 |
| Test eseguiti dal runner | 47 | 48 | +1 |
| Moduli standard | 4 | 7 | +3 |
| Codici errore | 0 | 15 | +15 |
| README righe | ~1989 | 2151 | +162 |
| Borrow checker features | 18/27 | 18/27 | +0 |

---

## 7. Bug noti e limitazioni

### 7.1 Expression-form match con pattern-bound variables

`var x = case adt { Variant(v): v, ... }` genera C con `v` non dichiarato. Il workaround è usare statement-form match. Bug nell'emitter, non nel type system.

### 7.2 `*u8` vs `char*` interop

Lain emette `*u8` come `uint8_t*` ovunque. Le funzioni C string richiedono `char*`. Soluzioni possibili:
- Tipo nativo `char` che mappa a `char` in C
- Attributo `@c_string` sul parametro
- Regola speciale nell'emitter per `*u8` in contesto extern

### 7.3 Match su struct non-ADT

`case` su una struct semplice (non-enum, non-ADT) causa un crash (SIGABRT). Bug pre-esistente, non introdotto da queste modifiche.

### 7.4 `case &` su espressioni complesse

Il borrow condiviso è registrato solo se il scrutinee è un `EXPR_IDENTIFIER` diretto. `case &obj.field { ... }` non registra il borrow (il valore non viene comunque consumato, ma non c'è protezione contro la mutazione).

---

## 8. Roadmap futura

### 8.1 Prossimi passi immediati

1. **Fix expression-form match codegen** — L'emitter deve dichiarare le variabili pattern-bound prima di usarle nell'expression match. ~30 righe in emit/expr.h.

2. **`case &` per espressioni complesse** — Estendere il borrow registration a EXPR_MEMBER e altri tipi di espressione. ~20 righe in linearity.h.

3. **Tipo `char` nativo** — Sblocca std/string.ln e l'interop con funzioni C string. Richiede modifiche a lexer, parser, type system, e emitter.

### 8.2 Medio termine

4. **Operatore `?` per error propagation** — Desugara `expr?` in `case expr { Ok(v): v, Err(e): return Err(e) }`. Richiede che il tipo di ritorno sia un Result.

5. **Diagnostica multi-span** — Mostrare sia il punto di errore che il punto di dichiarazione/move originale. Estensione di `diagnostic_show_line()`.

6. **`std/mem.ln`** — Arena allocator nativo con ownership lineare.

### 8.3 Lungo termine

7. **Closures con capture modes** — Cattura per valore (`mov`), per reference (`&`), per mutable reference (`var &`).

8. **Traits/Interfaces** — Polimorfismo ad-hoc. Decisione architetturale aperta.

9. **Tuple types** — `(int, bool)` come tipo first-class per return multipli.

---

## 9. Conclusione

Questa sessione ha consolidato il compilatore Lain su tre fronti: libreria standard generica (Option/Result), sicurezza del borrowed match (borrow persistente), e developer experience (error codes + documentazione). Il README è ora una specifica completa e aggiornata di 2151 righe che copre tutte le feature implementate.

Il prossimo salto qualitativo viene dal fix dell'expression-form match (codegen) e dall'introduzione di un tipo `char` nativo per sbloccare l'interop stringa — due feature relativamente piccole ma con alto impatto sull'usabilità quotidiana del linguaggio.

---

*Fine dell'analisi — 6 marzo 2026*
