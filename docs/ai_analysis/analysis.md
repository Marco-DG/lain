# Lain Language — Problem Analysis & Roadmap

**Author**: Analisi automatica basata sulla lettura completa del codice sorgente del compilatore, dei test, delle specifiche e della documentazione.

---

## Sommario Esecutivo

Lain è un linguaggio con fondamenta solide — la filosofia core (purity, linear types, static bounds) è ben definita e le feature principali funzionano. Tuttavia, il progetto è in fase prototipale e presenta **problemi strutturali nel compilatore**, **lacune semantiche nel linguaggio**, e **inconsistenze** che devono essere risolte prima di poter considerare il linguaggio "stabile".

Questa analisi è organizzata in **4 categorie** di severità, seguite da una **roadmap prioritizzata**.

---

## 🔴 Categoria A: Bug e Problemi Critici

Problemi che causano comportamenti incorretti, crash, o falle nella safety.

---

### A1. `break` non ha un token dedicato

**Problema**: `break` e `continue` sono usati nei test (`while_loop.ln`) e funzionano a runtime, ma **non esiste** `TOKEN_KEYWORD_BREAK` né `TOKEN_KEYWORD_CONTINUE` nel lexer (`token.h`). Questo significa che il parser li gestisce probabilmente come identificatori raw, il che è fragile e può causare collisioni di nome.

**File coinvolti**: [token.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/token.h)

**Fix**: Aggiungere `TOKEN_KEYWORD_BREAK` e dedicare un percorso di parsing esplicito nel parser. Attualmente, se un utente dichiara una variabile `break`, il comportamento è indefinito.

**Severità**: 🔴 Critica — keyword fondamentali trattate come identificatori.

---

### A2. Scope system piatto — nessun block scoping

**Problema**: Il sistema di scope in [scope.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/scope.h) usa solo **due tabelle hash**: `sema_globals[]` e `sema_locals[]`. Non esiste il concetto di **scope annidato** (block-level scoping).

```c
// scope.h — solo due tabelle
static Symbol *sema_globals[SEMA_BUCKET_COUNT];
static Symbol *sema_locals [SEMA_BUCKET_COUNT];
```

**Conseguenze**:
- Variabili dichiarate dentro un `if`/`for`/`while` block sono visibili **fuori** dal blocco.
- Ri-dichiarare una variabile in un blocco diverso può causare collisioni.
- Lo shadowing non funziona correttamente.

**Fix**: Implementare uno stack di scope o scope linkati (scope tree) dove ogni `{...}` crea un nuovo livello.

**Severità**: 🔴 Critica — viola le aspettative semantiche di qualsiasi linguaggio con blocchi.

---

### A3. Bounds checker incompleto sugli array dinamici

**Problema**: In [bounds.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/bounds.h), quando la lunghezza dell'array è sconosciuta (slice dinamico), il checker **non emette alcun errore né warning**:

```c
// bounds.h, linea 77-81
} else {
    // Index unknown -> potentially unsafe
    // Erroring here might be too aggressive...
}
```

Questo contraddice la filosofia "Zero Runtime Checks" del linguaggio. Un accesso a indice sconosciuto su un array dinamico passa **silenziosamente** senza verifica.

**Opzioni di fix**:
1. **Strict**: Rifiutare qualsiasi accesso che non può essere provato safe staticamente.
2. **Pragmatic**: Richiedere il constraint `in` per accessi dinamici (come documentato, ma non enforced).

**Severità**: 🔴 Critica — falla nella safety promise del linguaggio.

---

### A4. Linearity checker: TODO sulle varianti enum con dati

**Problema**: In [linearity.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/linearity.h) (linea 86), il check di linearità sulle varianti ADT/enum con campi è un TODO:

```c
// TODO: Enums (variants with fields)
```

Se un ADT contiene un campo lineare (`mov`), il compilatore **non lo rileva** come tipo lineare. Questo significa che variant con risorse `mov` possono essere copiate o forgottane senza errore.

**Severità**: 🔴 Critica — viola la garanzia di linearità.

---

### A5. Mapping C hardcoded nel codice emitter

**Problema**: Come documentato nella spec ([012_opaque_types_impl.md](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/../specs/012_opaque_types_impl.md)), i mapping dei tipi C sono **hardcoded** nell'emitter tramite whitelist di nomi di funzione (`fopen`, `fputs`, etc.).

Qualsiasi funzione C non nella whitelist riceve i mapping sbagliati (e.g. `const FILE*` invece di `FILE*`).

**Severità**: 🟠 Alta — limita l'estensibilità della C interop.

---

### A6. Tutti i globali sono trattati come mutabili

**Problema**: In `scope.h` (linea 59):
```c
sym->is_mutable = true;  // Globals are mutable by default
```

Tutti i globali sono forzati a mutabili, anche se dichiarati senza `var`. Questo annulla la distinzione immutable/mutable per i globali.

**Severità**: 🟠 Alta — viola la semantica di immutabilità.

---

## 🟠 Categoria B: Lacune Semantiche del Linguaggio

Feature mancanti che un linguaggio con le ambizioni di Lain dovrebbe avere.

---

### B1. Nessun sistema di tipi interi definito

**Problema**: Lain dichiara `int`, `u8`, `usize` ma:
- Non specifica la dimensione di `int` (dipende dal compilatore C target)
- Non ha `i8`, `i16`, `i32`, `i64`, `u16`, `u32`, `u64`
- Non ha protezione contro l'overflow intero

Per un linguaggio "critical-safe" destinato all'embedded, la dimensione dei tipi deve essere **deterministica**.

**Raccomandazione**: Definire tipi a dimensione fissa (`i32`, `u32`, etc.). Rendere `int` un alias esplicito (e.g. `int = i32`).

---

### B2. Nessun tipo `bool` vero

**Problema**: `bool` appare come tipo nel linguaggio, ma i test usano `0`/`1` come booleani (stile C). Non è chiaro se `bool` sia un tipo fondamentale o un alias per `int`.

```lain
if 0 { ... }      // "False" con un intero
while 1 { ... }   // "True" con un intero
```

**Raccomandazione**: Decidere se `bool` è un tipo distinto con valori `true`/`false`, o se è un alias per `int`. Per la type safety, un tipo distinto è preferibile.

---

### B3. Nessun operatore address-of (`&`)

**Problema**: Come notato nel test `unsafe_valid.ln`:
```lain
// We don't have address-of operator '&' for local sets yet?
```

Non c'è modo di ottenere un puntatore a una variabile locale. Questo limita severamente l'interop C e il lavoro low-level.

**Raccomandazione**: Implementare `&x` come operazione unsafe che restituisce `*T`.

---

### B4. Nessun cast esplicito

**Problema**: La keyword `as` è riservata ma non implementata. Non c'è modo di convertire tra tipi (e.g. `u8` → `int`, `*void` → `*int`).

**Raccomandazione**: Implementare `x as T` per conversioni safe, e `unsafe { x as *T }` per cast di puntatori.

---

### B5. Nessun tipo stringa nativo

**Problema**: Le stringhe sono attualmente `u8[:0]` (sentinel slices). Questo è funzionalmente corretto ma semanticamente fragile:
- Non c'è distinzione tra "dati binari" e "testo"
- L'utente deve accedere a `.data` manualmente per passare a C
- Non c'è concatenazione, formattazione, o manipolazione di stringhe

**Raccomandazione (a lungo termine)**: Definire un alias `string = u8[:0]` nella stdlib e fornire operazioni base.

---

### B6. Nessuna gestione degli errori

**Problema**: Non esiste un meccanismo per segnalare e gestire errori:
- Nessun `Result<T, E>` o `Option<T>` (la spec ne discute ma non è implementato)
- Nessun `panic`, `assert`, o `unreachable`
- Le funzioni che possono fallire (`fopen`) restituiscono puntatori nulli senza wrapping

Come notato in `std/fs.ln`:
```lain
func open_file(path u8[:0], mode u8[:0]) mov File {
    var raw = fopen(path.data, mode.data)
    // TODO: Handle NULL return   ← !!!
    return File(raw)
}
```

**Raccomandazione**: Implementare `Option<T>` come ADT + niche optimization (come proposto nella spec `014_zero_cost_options_analysis.md`).

---

### B7. Nessun ciclo `for` su array/slice

**Problema**: Il `for` supporta solo range numerici (`0..n`). Non è possibile iterare direttamente su un array o uno slice:

```lain
// Non supportato:
for elem in array { ... }

// Attualmente serve:
for i in 0..5 {
    var elem = array[i]
}
```

**Raccomandazione**: Estendere `for` per accettare espressioni array/slice.

---

### B8. Nessun destructuring in `match` per struct

**Problema**: Il `match` supporta destructuring per ADT (`Circle(r): ...`) ma non per struct normali. Non c'è let-binding con destructuring:

```lain
// Non supportato:
var {x, y} = point
```

Il destructuring nei parametri (`mov {id} Resource`) è **parzialmente** implementato ma solo per il caso di consumo lineare.

---

## 🟡 Categoria C: Inconsistenze e Debito Tecnico

---

### C1. `match` vs `case` vs `switch`

**Problema**: Il token.h mappa **tre keyword** allo stesso token:
```c
if (strncmp(lexeme, "match", 5) == 0)  return TOKEN_KEYWORD_CASE;
if (strncmp(lexeme, "switch", 6) == 0) return TOKEN_KEYWORD_CASE;
```

E ci sono TODO nei commenti: `// TODO: to remove now it is called "case"`.

**Raccomandazione**: Decidere **un** keyword canonico e rimuovere gli alias. Suggerimento: mantieni `match` (è più espressivo e coerente con la documentazione attuale).

---

### C2. `libc_printf` vs `printf`

**Problema**: I test usano a volte `printf`, a volte `libc_printf`, a volte `libc_puts`. L'emitter ha logica speciale per mappare `libc_printf` → `printf` nel C generato, ma questa convenzione non è documentata e causa confusione.

**Raccomandazione**: Standardizzare: O usare sempre `printf` nelle dichiarazioni extern (più naturale), o documentare chiaramente la convenzione `libc_` e il perché.

---

### C3. Semicoloni inconsistenti nei test

**Problema**: Alcuni test usano il `;`, altri no, alcuni mescolano. Questo è "legale" ma causa confusione sulla natura del linguaggio.

```lain
// File 1: Senza semicoloni
var x = 10
return 0

// File 2: Con semicoloni
var x int = 42;
return mov p;
```

**Raccomandazione**: Prendere una posizione chiara: O i semicoloni sono **obbligatori** o sono **vietati** (come Go). Il comportamento "opzionale" è il peggiore dei mondi.

---

### C4. Allocazioni `malloc` nel compilatore senza cleanup

**Problema**: Il compilatore usa `malloc` liberamente per strutture interne (symbol table, linearity table, etc.) ma spesso non fa `free`. Per un progetto che predica la safety della memoria, il compilatore stesso ha leak.

```c
// scope.h: malloc senza arena
Symbol *sym = malloc(sizeof *sym);
// linearity.h: malloc per le entry
LEntry *e = (LEntry*)malloc(sizeof *e);
```

L'arena allocator esiste (`utils/arena.h`) ma non è usata consistentemente.

**Raccomandazione**: Migrare tutte le allocazioni del compilatore all'arena allocator.

---

### C5. Header-only architecture del compilatore

**Problema**: L'intero compilatore è implementato in file `.h` (header-only). Questo causa:
- Compilazioni lente (tutto riecompilato ogni volta)
- Namespace pollution
- Impossibilità di compilazione separata

Per un progetto che cresce, questa architettura diventerà insostenibile.

**Raccomandazione**: A lungo termine, migrare a `.c` + `.h` con compilazione separata.

---

### C6. Nessun line number nelle error message

**Problema**: Gli errori del compilatore stampano solo il nome della variabile, non la riga e il file:

```
sema error: linear variable 'myitem' was already used/consumed.
```

Dovrebbe essere:

```
error[E0382]: use of moved value `myitem`
  --> tests/safety/ownership/use_after_move_fail.ln:22:18
```

**Severità**: 🟡 Medio — l'usabilità del compilatore è gravemente compromessa.

---

## 🔵 Categoria D: Feature Future Desiderabili

---

### D1. Generics / Comptime

Come descritto nella spec `017_comptime_metaprogramming_analysis.md`, il linguaggio ha bisogno di un meccanismo per il codice generico. Senza generics, ogni tipo contenitore (`Option`, `Result`, `Vec`) deve essere duplicato manualmente.

### D2. Destructori impliciti (RAII)

Come notato nella spec `012_opaque_types_impl.md`, i tipi lineari devono essere chiusi manualmente (`close_file(mov f)`). L'RAII automatico (destructor chiamato a fine scope) eliminerebbe questa classe di errori.

### D3. Trait / Interfacce

Nessun meccanismo per il polimorfismo. Non c'è modo di definire un'interfaccia comune (e.g. `Printable`, `Closeable`).

### D4. Niche Optimization per enum

Come proposto nella spec `014_zero_cost_options_analysis.md`, `Option<*T>` dovrebbe avere la stessa dimensione di `*T` usando il valore nullo come "None".

### D5. Self-hosting

Il compilatore è scritto in C. L'obiettivo a lungo termine dovrebbe essere riscriverlo in Lain (self-hosting), come test definitivo del linguaggio.

### D6. LSP / Editor Support

Nessun supporto per editor (syntax highlighting, autocompletamento, go-to-definition).

---

## Roadmap Prioritizzata

### Fase 1: Stabilizzazione Critica ⚡
*Obiettivo: rendere le promise di safety reali*

| # | Task | Severità | Effort |
|---|------|----------|--------|
| 1 | Aggiungere `TOKEN_KEYWORD_BREAK` e `TOKEN_KEYWORD_CONTINUE` | 🔴 | Basso |
| 2 | Implementare block scoping nello scope system | 🔴 | Medio |
| 3 | Completare bounds check su accessi dinamici (richiedere `in` o rifiutare) | 🔴 | Medio |
| 4 | Completare linearity check sulle varianti ADT con dati `mov` | 🔴 | Medio |
| 5 | Fix: globali immutabili trattati come mutabili | 🟠 | Basso |
| 6 | Aggiungere line + file number in tutti gli error message | 🟡 | Medio |

### Fase 2: Completamento Linguistico 🧩
*Obiettivo: rendere il linguaggio usabile per progetti reali*

| # | Task | Effort |
|---|------|--------|
| 7 | Definire tipi interi a dimensione fissa (`i32`, `u32`, etc.) | Medio |
| 8 | Implementare `Option<T>` come ADT builtin | Alto |
| 9 | Implementare cast esplicito (`as`) | Medio |
| 10 | Rimuovere alias `switch`/`case`, standardizzare su `match` | Basso |
| 11 | Decidere e enforcare la policy sui semicoloni | Basso |
| 12 | Implementare `for elem in array` | Medio |
| 13 | Eliminare la whitelist hardcoded per il type mapping C | Alto |

### Fase 3: Evoluzione 🚀
*Obiettivo: linguaggio competitivo*

| # | Task | Effort |
|---|------|--------|
| 14 | Comptime metaprogramming (generics) | Molto Alto |
| 15 | RAII / Destructori impliciti | Alto |
| 16 | Bool come tipo distinto con `true`/`false` | Basso |
| 17 | Operatore `&` (address-of) | Medio |
| 18 | Riscrittura compilatore in Lain (self-hosting) | Enorme |

---

## Conclusione

Il cuore di Lain è forte: la combinazione di **linear types + static range analysis + purity distinction** è unica e potente. Tuttavia, il compilatore ha bisogno di **consolidamento** prima di aggiungere nuove feature. Le priorità assolute sono:

1. **Block scoping** — senza questo, il linguaggio non è corretto.
2. **Bounds check completo** — senza questo, la promise "zero buffer overflow" è falsa.
3. **Linearity completa su ADT** — senza questo, le risorse lineari possono leakare.
4. **Error messages con location** — senza questo, il compilatore è inutilizzabile su file grandi.

Concentrati sulla **Fase 1** prima di tutto. Ogni feature futura costruita su fondamenta rotte sarà rotta a sua volta.
