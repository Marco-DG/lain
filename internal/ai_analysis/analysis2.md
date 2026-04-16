# Lain Language — Deep Analysis & Improvement Plan (v2)

**Author**: Analisi basata sulla lettura completa di `specification.md`, tutti i 52+ file di test, il codice sorgente del compilatore (`token.h`, `scope.h`, `sema.h`, `ast.h`, emitter), la standard library (`std/c.ln`, `std/io.ln`, `std/fs.ln`), e l'analisi precedente (`analysis.md`).

**Data**: 2026-02-23

---

## Sommario Esecutivo

Il progetto Lain ha fatto progressi significativi dall'analisi precedente. Diverse feature riportate come mancanti in `analysis.md` sono state **implementate** nel compilatore ma **non aggiornate nella specifica**. Questo crea una disconnessione pericolosa tra documentazione e realtà. Il problema principale ora non è solo tecnico, ma **documentativo**: la specifica dice cose false sia in positivo (descrive `match` come keyword canonica, ma il compilatore usa `case`) sia in negativo (dice che `bool`, `as`, `i32`, `i64` non sono implementati, ma lo sono).

Questa analisi è organizzata in **5 sezioni**:
1. 🔴 Divergenze Spec↔Implementazione (la specifica dice il falso)
2. 🟠 Problemi Semantici del Linguaggio
3. 🟡 Problemi nei Test
4. 🔵 Lacune nella Specifica (cose non documentate)
5. 📋 Roadmap Prioritizzata

---

## 🔴 1. Divergenze Specifica ↔ Implementazione

Problemi dove la specifica descrive un comportamento diverso da quello reale del compilatore. **Questi rendono la specifica inaffidabile e devono essere corretti con priorità assoluta.**

---

### 1.1 `match` vs `case` — La Keyword Sbagliata nella Specifica

**Stato nella specifica**: `match` è la keyword canonica per il pattern matching (§7.5, §7.6, §2.1, grammar).

**Stato reale nel compilatore**: La keyword `match` **NON ESISTE** nel lexer. Il token `TOKEN_KEYWORD_CASE` è mappato solo dalla stringa `"case"` (len 4, riga 122 di `token.h`). Non c'è *nessun* mapping per `"match"` (len 5) né per `"switch"` (len 6).

**Prova**: Tutti i test del progetto usano `case`, non `match`:
```lain
// tests/types/enums.ln
case c {
    Red: libc_printf("Color is Red\n")
    ...
}

// tests/types/adt.ln
case c {
    Circle(rad): libc_printf("Circle radius: %d\n", rad)
    ...
}

// tests/exhaustive_fail.ln
case x {
    1: return 1
    ...
}
```

**Impatto**: Un utente che legge la specifica e scrive `match x {}` otterrà un errore di parsing. La specifica è **completamente fuorviante** su questo punto.

**Correzione necessaria**: Decidere UNA keyword canonica, aggiornare specifica E compilatore per essere coerenti. La raccomandazione è:
- **Opzione A** (consigliata): Mantenere `case` nel compilatore, aggiornare la specifica.
- **Opzione B**: Aggiungere `match` al lexer, deprecare `case`, aggiornare la specifica per documentare entrambi durante la transizione.

> [!CAUTION]
> Questo è il problema più urgente. Qualsiasi utente che segue la specifica non potrà scrivere codice funzionante.

---

### 1.2 `bool`, `true`, `false` — Implementati ma Documentati come Mancanti

**Stato nella specifica**: §3.1 menziona `bool` come tipo primitivo con nota "*Boolean value*" ma il `WARNING` a §B2 dell'analisi precedente dice: "Non è chiaro se `bool` sia un tipo fondamentale o un alias per `int`". La specifica non documenta `true`/`false` come keyword.

**Stato reale nel compilatore**:
- `TOKEN_KEYWORD_TRUE` esiste (riga 80 di `token.h`)
- `TOKEN_KEYWORD_FALSE` esiste (riga 81 di `token.h`)
- Il lexer riconosce `"true"` (len 4, riga 124) e `"false"` (len 5, riga 128)

**Prova**: Il test `tests/types/bool_test.ln` usa queste feature con successo:
```lain
var x bool = true
var y bool = false

if x { libc_printf("x is true\n") }
libc_printf("true = %d, false = %d\n", true, false)
```

**Correzione necessaria**: Aggiornare §3.1 per specificare `bool` come tipo distinto con valori `true`/`false`. Aggiungere `true`/`false` alla keyword list in §2.1 e nell'Appendix A.

---

### 1.3 `as` (Type Cast) — Implementato ma Documentato come "Reserved"

**Stato nella specifica**: §B4 elenca `as` come "🔮 Reserved" e l'Appendix A lo marca come non implementato.

**Stato reale nel compilatore**:
- `TOKEN_KEYWORD_AS` esiste (riga 57 di `token.h`)
- Il lexer lo riconosce (len 2, riga 101)

**Prova**: Il test `tests/types/cast_test.ln` usa `as` con successo:
```lain
var x i32 = 1000
var y = x as u8
var big = 42 as i64
```

**Correzione necessaria**: Documentare `as` come keyword implementata in §2.1, aggiungere una sezione nella specifica (§8.6 o simile) che definisca la semantica del cast.

---

### 1.4 Tipi Interi Estesi — Implementati ma Documentati come Mancanti

**Stato nella specifica**: §3.1 elenca solo `int`, `u8`, `usize`, `bool`. Il `WARNING` dice: "Additional integer types (`i8`, `i16`, `i32`, `i64`, `u16`, `u32`, `u64`) are not yet defined".

**Stato reale nel compilatore**: I tipi `i32`, `i64`, `u32` sono riconosciuti e funzionano.

**Prova**: Il test `tests/types/integer_types.ln`:
```lain
var a i32 = 42
var b u8 = 255
var c i64 = 1000000
var d u32 = 12345
```

**Correzione necessaria**: Aggiornare §3.1 con la tabella completa dei tipi interi supportati, inclusi i mapping C99 (`int32_t`, `int64_t`, `uint32_t`). Specificare quali tipi sono operativi e quali ancora mancanti.

---

### 1.5 `while` in `func` — La Specifica lo Vieta, il Compilatore lo Permette

**Stato nella specifica**: §6.1 e §6.5 esplicitamente vietano `while` nelle funzioni pure:
> - ❌ Banned (while loops in func)
> - "Can only use `for` loops (over finite ranges). `while` loops are banned."

**Stato reale nel compilatore**: Il test `tests/core/while_loop.ln` usa `while` dentro `func main()` e compila senza errori:
```lain
func main() int {
    var i = 0
    while i < 10 {
        libc_printf("%d ", i)
        i = i + 1
    }
    ...
}
```

**Domanda di design**: Questo è un bug nel compilatore o una decisione consapevole? Il `main()` potrebbe essere un caso speciale (essendo sempre impuro per definizione), ma la specifica non lo documenta. Se `main` è speciale, va documentato.

**Correzione necessaria**: O il compilatore deve rifiutare `while` in `func`, O la specifica deve documentare le eccezioni (e.g., `main` è implicitamente un `proc`).

---

### 1.6 `func` vs `proc` — `main` è Dichiarato Inconsistentemente nei Test

Nei test, `main` è dichiarato a volte come `func`, a volte come `proc`:

| Test File | Dichiarazione |
|:----------|:-------------|
| `while_loop.ln` | `func main() int` |
| `functions.ln` | `proc main() int` |
| `math.ln` | `func main() int` |
| `control_flow.ln` | `func main() int` |
| `func_proc.ln` | `proc main() int` |
| `termination_pass.ln` | `proc main() int` |
| `borrow_pass.ln` | `func main() int` |
| `test_fs.ln` | `proc main() int` |

Il `main` che chiama `libc_printf` (che è I/O, un side effect) dovrebbe essere `proc`, non `func`. Il fatto che il compilatore accetti `func main()` che fa I/O suggerisce che **il purity check non è applicato su main**, o che le extern funcs non sono considerate proc.

**Correzione necessaria**: 
1. Specificare nella spec se `main` è implicitamente `proc` o se è un'eccezione alla purity rule.
2. Decidere se le funzioni `extern func` (come `libc_printf`) sono considerate pure o impure.

---

### 1.7 `extern func` vs `extern proc`

**Problema sottile**: In `tests/core/func_proc.ln`, `libc_printf` è dichiarato come `extern proc`:
```lain
extern proc libc_printf(fmt *u8, ...) int
```

Ma in quasi tutti gli altri test è dichiarato come `extern func`:
```lain
extern func libc_printf(fmt *u8, ...) int
```

Se `extern func` implica purezza, allora `libc_printf` (che fa I/O) non dovrebbe essere `func`. Ma se gli `extern` non sono soggetti al purity check (logico, dato che il compilatore non può verificare codice C), allora `func` vs `proc` per gli extern è puramente documentativo.

**Correzione necessaria**: La specifica deve chiarire la semantica di `extern func` vs `extern proc` — sono soggetti al purity check? Se no, qual è la differenza?

---

## 🟠 2. Problemi Semantici del Linguaggio

Problemi nel design del linguaggio stesso che richiedono decisioni architetturali.

---

### 2.1 Scope System Piatto (Confermato dall'Analisi Precedente)

Come già riportato in `analysis.md` A2, il sistema di scope in `scope.h` usa solo **due tabelle hash**: `sema_globals[]` e `sema_locals[]`. Non esiste il concetto di block scoping.

```c
// scope.h — conferma: solo clear_globals e clear_locals, nessun push/pop scope
static Symbol *sema_globals[SEMA_BUCKET_COUNT];
static Symbol *sema_locals [SEMA_BUCKET_COUNT];
```

`sema_clear_locals()` è chiamato solo all'ingresso/uscita di funzione, non ai blocchi.

**Impatto**: Variabili dichiarate dentro `if`/`for`/`while` sono visibili fuori dal blocco. Questa è una semantica incorretta per qualsiasi linguaggio moderno.

---

### 2.2 Bounds Checker — Accessi Dinamici Non Verificati (Confermato)

Il problema A3 dell'analisi precedente è confermato. Il test `range_loop_unsound.ln` dimostra la debolezza:

```lain
func main() int {
    var x = 0
    for i in 0..10 { x = x + 1 }
    // x is actually 10, but analysis can't prove it
    return require_one(x)  // Should fail: can't prove x == 1
}
```

La specifica documenta questa limitazione (§9.7 Loop Widening) ma non specifica cosa succede quando il widening rende impossibile verificare un constraint.

---

### 2.3 Linearità nelle Varianti ADT (Confermato)

Il problema A4 dell'analisi precedente è confermato. Nessun test copre il caso di un ADT con variant contenenti campi `mov`.

---

### 2.4 Il Problema `libc_printf` / `printf` / `-Dlibc_printf=printf`

**Scoperta**: Il file `run_tests.sh` usa un **trucco del preprocessore C** per risolvere il problema:

```bash
./cosmocc/bin/cosmocc out.c -o out/test.exe -w -Wno-pointer-sign -Dlibc_printf=printf -Dlibc_puts=puts
```

Il flag `-Dlibc_printf=printf` rimpiazza ogni occorrenza di `libc_printf` con `printf` nel C generato. Questo significa:
- L'emitter Lain genera codice C con `libc_printf(...)` 
- Il compilatore C lo trasforma in `printf(...)` via macro

Questo è un **hack pragmatico** ma fragile:
- Non funziona per funzioni con nomi non standard
- Un utente che compila senza quel flag otterrà link errors
- Nasconde il vero mapping tipo tra Lain e C

**La specifica non documenta questo meccanismo.**

---

### 2.5 Mancanza dell'Operatore Address-Of (`&`)

Come notato nel test `unsafe_valid.ln`:
```lain
// We don't have address-of operator '&' for local sets yet?
```

Senza `&`, non è possibile creare puntatori a variabili locali, limitando gravemente l'interop C e l'utilità di `unsafe`.

Il `TOKEN_AMPERSAND` esiste nel lexer, ma è usato solo per bitwise AND e `&=`. Non c'è un uso unario prefix per address-of.

---

### 2.6 `return var` / `return mov` — Semantica Non Documentata

Il test `syntax_check.ln` usa una sintassi non documentata nella specifica:

```lain
func ret_var() var int {   // Return type annotated with 'var'
    var x = 10
    return var x           // Return with 'var' annotation
}
```

La specifica (§5.7) documenta `return mov` ma NON `return var`. Cosa significa restituire un mutable reference? Il chiamante riceve un puntatore? Questa semantica deve essere chiarita.

---

### 2.7 Immutable Binding Syntax — `x = 10` vs `var x = 10`

La specifica (§4.1) documenta che `x = 10` senza `var` crea un binding immutabile:
```lain
x = 10           // Immutable binding
```

Questo è confermato dal test `00_immutability.ln`:
```lain
x = 10
// x = 20      // ERROR: Cannot assign to immutable variable
```

**Problema**: Questa sintassi è **ambigua** con un assignment a una variabile esistente. Come distingue il parser tra:
1. `x = 10` → nuova variabile immutabile `x`
2. `x = 10` → assegnamento alla variabile mutabile `x` già esistente

La regola implicita sembra essere: "se `x` non esiste, è una dichiarazione; se esiste, è un assegnamento". Questo deve essere **documentato esplicitamente** nella specifica.

---

## 🟡 3. Problemi nei Test

---

### 3.1 Test che Usano `printf` Senza Dichiarazione Extern

Il test `tests/safety/ownership/ownership.ln` chiama `printf` senza dichiararlo:
```lain
func print_point(p Point) {
    printf("Point(%d, %d)\n", p.x, p.y)  // printf non è dichiarato!
}
```

Allo stesso modo, `tests/safety/bounds/bounds_pass.ln`:
```lain
printf("arr[0]=%d, arr[4]=%d, arr[3]=%d\n", a, b, c)  // Non dichiarato
```

Questo potrebbe funzionare per la permissività del compilatore C (warning, non errore in C89/C99), ma è **semanticamente sbagliato** nel contesto di Lain che dovrebbe richiedere dichiarazioni esplicite.

---

### 3.2 Test `purity_fail.ln` — Violazione Parzialmente Commentata

Il test `tests/safety/purity/purity_fail.ln` ha la prima violazione commentata:
```lain
func pure_add(a int, b int) int {
    // Violation 1: Calling a proc
    // impure_increment()        // ← COMMENTATA
    
    // Violation 2: Modifying global
    global_counter = 100         // ← attiva
    
    return a + b
}
```

Se il test fallisce solo per la Violation 2, non abbiamo evidenza che la Violation 1 (chiamare un proc da func) sia effettivamente enforced. Servirebbero test separati.

---

### 3.3 Test `test_extern.ln` — Ownership Violation sulla `malloc`

```lain
extern func malloc(size usize) mov *void
...
var ptr *void = malloc(10)  // malloc restituisce mov *void
                            // ma ptr è dichiarato var *void (non mov)
```

La `malloc` restituisce un puntatore owened (`mov *void`). Assegnarlo a `var ptr *void` senza `mov` dovrebbe essere un errore secondo la specifica sull'ownership. Il fatto che compili suggerisce che il linearity checker non verifica i return type `mov` sulle extern.

---

### 3.4 Test `while_loop.ln` — Esclude dalla Verifica di Purity

Come discusso in §1.5, questo test usa `while` dentro `func main()`, violando la regola di purezza documentata nella specifica.

---

### 3.5 Coverage dei Test — Aree Scoperte

| Area | Copertura | Note |
|:-----|:----------|:-----|
| Basic functions/proc | ✅ Buona | 3 test |
| While/for loops | ✅ Buona | 2 test |
| Arithmetic | ✅ Buona | 1 test |
| Structs (value) | ✅ Buona | 1 test |
| Enums (simple) | ✅ Buona | 2 test |
| ADT with variants | ✅ Buona | 1 test |
| String slices | ✅ Buona | 1 test |
| Bounds checking (static) | ✅ Eccellente | 14 test |
| Ownership/borrowing | ✅ Eccellente | 11 test |
| Bool type | ✅ | 1 test |
| Cast (`as`) | ✅ | 1 test |
| Integer types | ✅ | 1 test |
| Exhaustiveness | ✅ | 2 test |
| Purity enforcement | ⚠️ Parziale | 2 test, uno parzialmente commentato |
| Unsafe blocks | ✅ Buona | 3 test |
| Stdlib/extern | ✅ Buona | 6 test |
| **Slice operations** | ❌ Assente | Nessun test su array→slice, slice indexing |
| **Nested structs** | ❌ Assente | No test su struct dentro struct (non-lineare) |
| **ADT con dati lineari** | ❌ Assente | Nessun test su variant con `mov` fields |
| **Block scoping** | ❌ Assente | Nessun test verifica scope boundaries |
| **Multiple return paths** | ⚠️ Parziale | Solo bounds constraints, no general |
| **Bitwise operators** | ❌ Assente | Nessun test su `&`, `|`, `^`, `~` |
| **Compound assignments** | ❌ Assente | Nessun test su `+=`, `-=`, etc |
| **Char literals** | ❌ Assente | Nessun test su `'A'`, `'\n'` |
| **Module namespace** | ❌ Assente | No test su conflitti di nomi tra moduli |
| **Recursive data types** | ❌ Assente | Come si definisce una linked list? |
| **`for` con due variabili** | ❌ Assente | `for i, val in 0..10` non testato |
| **`func` chiama `proc`** | ❌ Assente | Nessun test verifica che fallisca |
| **Immutable globals in func** | ❌ Assente | No test su accesso globale da func pura |

---

## 🔵 4. Lacune nella Specifica

Aspetti del linguaggio che **non sono documentati affatto** nella specifica.

---

### 4.1 Come Funziona il Name Mangling C

L'emitter genera nomi C per evitare collisioni. La specifica non documenta la strategia di name mangling (e.g., `main_match_keyword_lexeme` nel `scope.h`).

### 4.2 Come il Compilatore Distingue Dichiarazioni da Assegnamenti

Il parser deve disambiguare `x = 10` (nuova variabile) da `x = 10` (assegnamento). La regola non è documentata.

### 4.3 Trattamento dei `void` Return

La specifica mostra funzioni void (`func greet(msg u8[:0]) {`), ma non specifica se `return` senza valore è obbligatorio, opzionale, o vietato.

### 4.4 Initialization di Array

Come si inizializza un array? La specifica mostra solo `var arr int[5]` seguito da assegnamenti individuali. Non c'è una sintassi per array literals (`[1, 2, 3]`).

### 4.5 Equality Check su Struct e ADT

Cosa succede con `struct1 == struct2`? La specifica non definisce se l'uguaglianza strutturale è supportata.

### 4.6 Conversioni Implicite

Il compilatore fa conversioni implicite tra tipi numerici (e.g., `u8` → `int`)? La specifica non lo dice.

### 4.7 Ordine di Valutazione degli Argomenti

L'ordine di valutazione degli argomenti in una chiamata a funzione è specificato? In C è undefined behavior. Lain lo definisce?

### 4.8 Il Tipo `void`

`*void` è documentato come tipo puntatore, ma `void` da solo non è mai menzionato. Si può dichiarare `var x void`?

### 4.9 Forward Declarations

Si possono usare tipi/funzioni prima che siano dichiarate nel file? Il compilatore fa forward declaration gathering?

### 4.10 Operator Precedence

La specifica non definisce la precedenza degli operatori. Qual è la precedenza di `and`/`or` rispetto a `==`/`<`?

---

## 📋 5. Roadmap Prioritizzata

### Fase 0: Correzione Documentazione ✅ COMPLETATA

Allineare la specifica alla realtà del compilatore. **Non aggiungere feature, non fixare bug — solo documentare quello che c'è.**

| # | Task | Tipo | Stato |
|---|------|------|-------|
| 0.1 | Cambiare `match` → `case` in tutta la specifica | Spec | ✅ |
| 0.2 | Documentare `bool` con `true`/`false` come tipo e keyword implementati | Spec | ✅ |
| 0.3 | Documentare `as` come operatore di cast implementato (§8.6) | Spec | ✅ |
| 0.4 | Documentare tipi `i8`–`i64`, `u8`–`u64`, `isize` in §3.1 | Spec | ✅ |
| 0.5 | Chiarire `main` deve essere `proc` + nota IMPORTANT in §6.4 | Spec | ✅ |
| 0.6 | Chiarire `extern func` vs `extern proc` — semantica in §6.5 | Spec | ✅ |
| 0.7 | Documentare il meccanismo `-Dlibc_printf=printf` (§14.5) | Spec | ✅ |
| 0.8 | Documentare `return var` / `return mov` in §5.7 | Spec | ✅ |
| 0.9 | Aggiungere sezione operator precedence (§8.7, 12 livelli) | Spec | ✅ |
| 0.10 | Documentare come `x = 10` si disambigua da assegnamento (§4.5) | Spec | ✅ |

### Fase 1: Fix Critici ✅ COMPLETATA (parziale)

| # | Task | Severità | Stato |
|---|------|----------|-------|
| 1.1 | Implementare block scoping (scope stack) | 🔴 | ✅ `scope.h` + `resolve.h` + `sema.h` |
| 1.2 | Completare bounds check su accessi dinamici | 🔴 | ⏳ Deferito |
| 1.3 | Completare linearity check su varianti ADT con dati `mov` | 🔴 | ✅ Già implementato (confermato `linearity.h:86-97`) |
| 1.4 | Fix: globali immutabili trattati come mutabili | 🟠 | ✅ Già implementato (`resolve.h:419-428`) |
| 1.5 | Aggiungere line + file in error messages | 🟡 | ⚠️ Parziale: purity/immutability OK, linearity no |
| 1.6 | Enforce purity check: `func` non deve chiamare `proc` | 🟠 | ✅ Già implementato (`resolve.h:573-583`) |
| 1.7 | Enforce purity check: `while` vietato in `func` | 🟠 | ✅ Implementato in `resolve.h` |

### Pre-Fase 2: Fix Scoperti Durante Fase 1 ⚠️ NUOVA

Durante l'implementazione della Fase 1 sono emersi **5 nuovi problemi** che devono essere affrontati prima di procedere con la Fase 2.

---

#### PF2.1 — Import System Non Preserva le Annotazioni `mov` (CRITICO)

**Scoperta**: Il sistema di import (moduli inlining) **non preserva** le annotazioni di ownership ai call-site. Quando una funzione importata viene "inlinata", il corpo della funzione sostituisce la chiamata, ma le annotazioni `mov` degli argomenti al call-site vengono perse.

**Esempio concreto**:
```lain
// In main():
close_file(mov f)     // ← mov annotato dal programmatore

// Dopo inlining, il linearity checker vede:
fclose(f.handle)      // ← il 'mov' è perso; f.handle è un accesso a member, non un consumo
```

**Impatto**: È **impossibile** estendere il linearity checker a `proc` finché il sistema di import non preserva le annotazioni. Qualsiasi `proc` che chiama funzioni importate con parametri `mov` genererà false positive.

**Fix necessario**: Modificare il sistema di import/inlining per:
- Preservare i `DECL_FUNCTION`/`DECL_PROCEDURE` delle funzioni importate nella decl list, oppure
- Non inlinare il corpo delle funzioni importate, ma risolvere solo i nomi, oppure
- Aggiungere un pass che propaga le informazioni di ownership dai siti di chiamata ai parametri.

**Severità**: 🔴 Bloccante per linearity su `proc`.

---

#### PF2.2 — Linearity Fallback Fragile per Funzioni Importate

**Scoperta**: Quando il linearity checker non trova la dichiarazione di una funzione chiamata (perché proviene da un modulo importato), usa un **fallback euristico** per decidere se consumare gli argomenti.

**Stato precedente**: Il fallback consumava QUALSIASI argomento con tipo `mov` → false positive su `write_file(f, content)` dove `f` è `mov File` ma il parametro è `var f File` (mutable borrow, non move).

**Fix applicato**: Il fallback ora consuma solo:
1. Argomenti esplicitamente wrappati in `EXPR_MOVE` (`mov x`)
2. Argomenti a struct constructor calls con tipo lineare (`File(raw)`)

**Rischio residuo**: Funzioni importate con parametri `mov` che vengono chiamate SENZA `mov` esplicito non saranno verificate. Il fix corretto è PF2.1 (preservare le decl importate).

**Severità**: 🟠 Mitigato ma non risolto completamente.

---

#### PF2.3 — `close_file` in `std/fs.ln` Ha un Bug di Linearità

**Scoperta**: La funzione `close_file` consuma il parametro `mov f File` solo in un branch:
```lain
proc close_file(mov f File) {
    if f.handle != 0 {    // ← f consumato solo nel branch true
        fclose(f.handle)
    }                     // ← branch false: f non consumato → LEAK
}
```

**Impatto**: Questo è un **bug reale di linearità** che il checker attuale non rileva (perché non gira su `proc`). Quando PF2.1 sarà risolto e il linearity checker esteso a `proc`, questo errore emergerà.

**Fix**: Rimuovere il guard `if f.handle != 0` oppure aggiungere un `else` branch che consuma `f`. La versione corretta sarebbe: `fclose(f.handle)` direttamente, oppure un pattern safe con `else { drop(f) }` quando `drop` sarà implementato.

**Severità**: 🟡 Non bloccante ora, ma emergerà in futuro.

---

#### PF2.4 — Test Files Usano `func main()` con I/O

**Scoperta**: ~40 test files dichiarano `func main()` ma chiamano `libc_printf` (dichiarato come `extern func`, quindi non viola il purity check). Se `libc_printf` venisse correttamente dichiarato come `extern proc`, **tutti questi test fallirebbero** perché `func main()` non potrebbe chiamare un `proc`.

**Stato attuale**: I test funzionano perché `libc_printf` è dichiarato `extern func` (trusted as pure), il che è **semanticamente falso** (printf ha side effects).

**Fix necessario**: Decidere la policy:
1. Mantenere `extern func libc_printf` come "trusted pure" (pragmatico, ignora la realtà)
2. Cambiare tutti i test a `proc main()` e `extern proc libc_printf` (corretto ma massiccio)
3. Rendere `main` implicitamente `proc` nel compilatore (suggerito)

**Severità**: 🟡 Non bloccante, ma incoerente.

---

#### PF2.5 — Linearity Errors Senza Line/Col

**Scoperta**: Gli errori emessi dal linearity checker (in `linearity.h`) non includono informazioni di riga e colonna:
```
sema error: linear variable 'w1' was not consumed before return.
sema error: use of linear variable 'myitem' after it was moved
```

Questo perché `LEntry` non memorizza la posizione sorgente della variabile. Gli errori di purity e immutabilità invece includono già `Ln`/`Col`:
```
Error Ln 8, Col 6: 'while' loops are not allowed in pure function 'loop_forever'.
Error Ln 3, Col 2: Cannot assign to immutable variable 'x'
```

**Fix necessario**: Aggiungere campi `isize line, col;` a `LEntry` in `linearity.h`, popolarli da `Stmt->line`/`Stmt->col` quando si registra la variabile, e usarli nei messaggi di errore.

**Severità**: 🟡 Quality-of-life, non bloccante.

### Fase 2: Test Coverage

| # | Task | Effort |
|---|------|--------|
| 2.1 | Aggiungere test per bitwise operators | Basso |
| 2.2 | Aggiungere test per compound assignments | Basso |
| 2.3 | Aggiungere test per char literals | Basso |
| 2.4 | Aggiungere test per `for i, val in 0..10` | Basso |
| 2.5 | Aggiungere test separato per "func chiama proc → errore" | Basso |
| 2.6 | Aggiungere test per slice operations (se implementate) | Medio |
| 2.7 | Aggiungere test per ADT con `mov` fields | Medio |
| 2.8 | Aggiungere test per block scoping (dopo fix) | Medio |
| 2.9 | Fix `ownership.ln` e `bounds_pass.ln`: aggiungere `extern func printf` | Basso |

### Fase 3: Completamento Feature

| # | Task | Effort |
|---|------|--------|
| 3.1 | Implementare address-of (`&x`) in `unsafe` | Medio |
| 3.2 | Implementare`Option<T>` come ADT | Alto |
| 3.3 | Eliminare whitelist hardcoded per il type mapping C nell'emitter | Alto |
| 3.4 | Standardizzare su `match` o `case` (rimuovere l'altra keyword) | Basso |
| 3.5 | Decidere e enforcare policy sui semicoloni | Basso |
| 3.6 | Implementare `for elem in array` | Medio |

### Fase 4: Evoluzione

| # | Task | Effort |
|---|------|--------|
| 4.1 | Comptime metaprogramming (generics) | Molto Alto |
| 4.2 | RAII / Destructori impliciti | Alto |
| 4.3 | Trait / Interfacce | Alto |
| 4.4 | Self-hosting | Enorme |

---

## Conclusione

La divergenza principale rispetto all'analisi precedente (`analysis.md`) è la scoperta che **diverse feature riportate come mancanti sono state implementate** senza aggiornare la documentazione. Questo crea una situazione pericolosa dove la specifica è inaffidabile come riferimento.

Le priorità **immediate** sono:
1. **Fase 0**: Allineare la specifica alla realtà — documentare `case`, `bool`, `as`, `i32`/`i64`, chiarire `func main()` vs `proc main()`.
2. **Fase 1**: Completare le garanzie di safety — block scoping, bounds check completo, linearity su ADT.
3. **Fase 2**: Espandere i test per coprire le aree scoperte.

L'analisi precedente rimane valida per i punti non corretti (A2-A6, B1-B8, C1-C6), ma i punti A1 (break/continue non tokenizzati) e B2/B4 (bool/as non implementati) sono **obsoleti** — queste feature sono ora implementate nel compilatore.
