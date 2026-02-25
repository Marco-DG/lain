# Lain Language — Deep Analysis & Improvement Plan (v3)

**Autore**: Analisi basata sulla lettura completa di `specification.md` (v0.1.0, 1643 righe), tutti i ~52 file di test, la standard library (`std/c.ln`, `std/io.ln`, `std/fs.ln`), lo script `run_tests.sh`, il progetto `Lain_1/`, e le analisi precedenti (`analysis.md`, `analysis2.md`).

**Data**: 2026-02-25

---

## Sommario Esecutivo

Dall'analisi precedente (`analysis2.md`, 2026-02-23) sono stati fatti **progressi significativi**:

- ✅ La specifica è stata riscritta e ampliata (da ~800 righe a 1643 righe)
- ✅ Tutte le divergenze spec↔implementazione della Fase 0 sono state corrette
- ✅ Block scoping implementato (`block_scope_fail.ln` test presente)
- ✅ Purity enforcement migliorato: 3 test (inclusi `while_in_func_fail.ln` e `repro_purity_fail.ln`)
- ✅ `close_file` in `std/fs.ln` corretto con destructuring: `proc close_file(mov {handle} File)`
- ✅ Tutti i core test ora usano `proc main()` coerentemente
- ✅ Operator precedence documentata (§8.7)
- ✅ Binding vs assignment disambiguation documentata (§4.5)
- ✅ `return var` / `return mov` documentati (§5.7)
- ✅ Address-of `&` documentato come `unsafe`-only (§12.4)

**Questa analisi si concentra su ciò che rimane**, organizzato in 6 sezioni:
1. 🔴 Problemi Critici nella Specifica (ambiguità, contraddizioni, lacune gravi)
2. 🟠 Problemi Semantici del Linguaggio (design decisions da prendere)
3. 🟡 Problemi nei Test (coverage gaps, test errati)
4. 🔵 Lacune nella Specifica (aspetti non documentati)
5. 🟣 Problemi nella Standard Library
6. 📋 Roadmap Prioritizzata

---

## 🔴 1. Problemi Critici nella Specifica

Problemi che rendono la specifica **ambigua, contraddittoria, o insufficiente** per un utente che voglia scrivere codice Lain corretto.

---

### 1.1 La Specifica di `std/c.ln` È Falsa — `fopen`/`fclose`/`fputs` Dichiarate `func` Anziché `proc`

**Stato nella specifica (§10.2)**: La specifica mostra `std/c.ln` con le dichiarazioni corrette:
```lain
extern func printf(fmt *u8, ...) int
extern func fopen(filename *u8, mode *u8) mov *FILE
extern func fclose(stream mov *FILE) int
extern func fputs(s *u8, stream *FILE) int
```

**Stato reale in `std/c.ln`**: Identico al codice in specifica — tutte dichiarate come `extern func`.

**Il problema**: `fopen`, `fclose`, e `fputs` sono **intrinsecamente impure** — effettuano I/O su file system. Dichiararle come `extern func` (pure) è **semanticamente falso**. Una funzione `func` che chiama `fopen` dovrebbe essere un errore di purità, ma non lo è perché `fopen` è dichiarata `func`.

**Impatto**: Un utente potrebbe scrivere una funzione `func` che apre/chiude file senza che il compilatore segnali errori di purità. Questo viola la garanzia fondamentale di Lain che le `func` sono pure e prive di side effects.

**Opzioni di correzione**:
- **Opzione A** (consigliata): Dichiarare `fopen`, `fclose`, `fputs`, `fgets` come `extern proc` in `std/c.ln` e aggiornare la specifica di conseguenza.
- **Opzione B**: Introdurre un terzo livello `extern unsafe func` che indica "trusted pure ma potenzialmente unsafe". Troppo complesso per ora.

> [!CAUTION]
> Questo è il singolo problema più grave nella specifica attuale. Mina la garanzia di purità che è uno dei pilastri del linguaggio.

---

### 1.2 `open_file` È `func` Ma Chiama `fopen` — Doppia Violazione

**In `std/fs.ln`**:
```lain
func open_file(path u8[:0], mode u8[:0]) mov File {
    var raw = fopen(path.data, mode.data)
    return File(raw)
}
```

`open_file` è dichiarata `func` (pura), ma:
1. Chiama `fopen` che effettua I/O (side effect)
2. Dovrebbe essere `proc open_file(...)` per coerenza con il modello di purità

Se `fopen` viene corretta a `extern proc` (come suggerito in §1.1), allora `open_file` deve diventare `proc` perché una `func` non può chiamare un `proc`.

**Nella specifica (§10.2)**: Il codice di `std/fs.ln` mostrato usa `func open_file` — deve essere aggiornato.

---

### 1.3 Nessuna NULL Safety su `fopen` — La Specifica Lo Ignora

**In `std/fs.ln`**:
```lain
func open_file(path u8[:0], mode u8[:0]) mov File {
    var raw = fopen(path.data, mode.data)
    // TODO: Handle NULL return          ← commento nel codice reale
    return File(raw)
}
```

Il `TODO` nel codice è pericoloso: `fopen` può restituire `NULL` in C se il file non esiste o non è accessibile. Il risultato viene wrappato in `File(raw)` senza controllo, creando un handle invalido.

**La specifica non menziona questo problema.** Un linguaggio che promette "memory safety" e "null dereference prevented" (§13) non può avere una stdlib che non gestisce NULL.

**Correzione necessaria**: 
1. Aggiungere un controllo NULL in `open_file` (richiede un modo per testare `*void == 0`)
2. Opzionalmente, introdurre un tipo `Option` / `Result` per gestire l'errore (Fase 3+)
3. Documentare nella specifica che il NULL check manca e come sarà risolto

---

### 1.4 Semantica Ambigua di `u8[]` (Slice Senza Sentinel) — Mai Usato Nei Test

La specifica (§3.4) documenta i tipi slice `int[]` e i sentinel slices `u8[:0]`, ma:

1. **Nessun test usa slice non-sentinel** (`int[]`, `u8[]`, ecc.)
2. Il progetto `Lain_1/src/main.ln` usa `lexeme u8[]`, indicando che il tipo esiste nel parser
3. Non è documentato come creare uno slice da un array (`arr[0..3]` è mostrato in §3.4 ma mai testato)
4. Non è chiaro come si accede agli elementi di uno slice, o come si passa uno slice a una funzione

**Impatto**: Gli slice sono una feature fantasma — documentata ma probabilmente non implementata o non funzionante. Un utente che prova a usarli potrebbe incontrare errori criptici.

---

### 1.5 Il Tipo `void` e `*void` — Semantica Incompleta

La specifica (§3.9) dice:
> `void` è usato esclusivamente per dichiarare puntatori opachi (`*void`)
> `var x void` è un errore di compilazione

Ma `test_extern.ln` mostra:
```lain
extern func malloc(size usize) mov *void
var ptr *void = malloc(10)
```

**Problemi non documentati**:
1. Come si fa il cast da `*void` a `*int`? La specifica documenta `as` per tipi numerici, ma non per puntatori
2. Il comparare `*void == 0` (per null check) funziona? È `unsafe`?
3. `free(ptr)` dopo `var ptr *void = malloc(10)` non usa `mov ptr` — ma `free` vuole `mov *void`. Questa è una **ownership violation** che il compilatore non cattura (vedi §3.3 di analysis2)

---

### 1.6 Shadowing — La Specifica Lo Vieta Ma Non È Testato

**Specifica (§4.6)**:
> Shadowing variables from an outer scope within an inner scope is strictly **forbidden**.

Ma:
1. **Non esiste un test `shadowing_fail.ln`** che verifica questo comportamento
2. Non è chiaro se il compilatore effettivamente rifiuta lo shadowing
3. La decisione di vietare lo shadowing è **più restrittiva di quasi tutti i linguaggi moderni** (Rust, Go, Zig lo permettono). Va motivata nella specifica

---

### 1.7 Manca la Sezione sulle Float — La Specifica Non Li Menziona

La specifica elenca solo tipi interi, `bool`, puntatori, array, slice, struct, ADT. **Non c'è nessuna menzione di tipi floating-point** (`f32`, `f64`, `float`, `double`).

Il linguaggio supporta i float? Se no, va dichiarato esplicitamente. Per un linguaggio embedded, la mancanza di float è legittima ma deve essere documentata con la motivazione (e.g., "determinism: floating-point is platform-dependent").

---

## 🟠 2. Problemi Semantici del Linguaggio

Decisioni di design che devono essere prese e documentate.

---

### 2.1 `extern func` vs `extern proc` — La Policy Non È Chiara

La specifica (§6.5, nota) ora dice:
> `extern func`: trusted to be pure (callable from `func`)
> `extern proc`: may have side effects (callable only from `proc`)

Ma nella pratica:
- `libc_printf` è `extern func` in `std/c.ln` (semanticamente falso: printf ha side effects)
- `fopen`/`fclose` sono `extern func` (falso: I/O è un side effect)
- Solo `libc_printf` è dichiarato `extern proc` in `functions.ln` e `func_proc.ln`

**Decisione necessaria**: Il sistema di purità è **opt-in** per le extern? Cioè, il programmatore decide se una funzione C è pura? Se sì, la responsabilità è del programmatore e va documentato con un warning prominente. Se no, tutte le funzioni I/O devono essere `extern proc`.

> [!IMPORTANT]
> Questa decisione impatta **tutta la standard library** e **tutti i test file**.

---

### 2.2 Linearità nelle Procedure — Ancora Parziale

Dalla analysis2 (§PF2.1, PF2.2):
> Il sistema di import non preserva le annotazioni `mov` delle funzioni importate

Il test `test_fs.ln` mostra:
```lain
write_file(f, content)     // f è `var File` (mutable borrow), non `mov`
close_file(mov f)          // f è `mov File` (ownership consumed)
```

Questo compila e funziona, ma la domanda è: **il linearity checker verifica correttamente le proc?** Il `close_file(mov f)` consuma `f`, ma il checker vede il `mov` esplicito al call-site? E se l'utente scrive `close_file(f)` senza `mov`, viene segnalato un errore?

**Servono test espliciti:**
- `close_file_without_mov_fail.ln` — deve fallire se manca `mov`
- `double_close_fail.ln` — deve fallire se `f` viene usato dopo `close_file(mov f)`
- `forgot_close_fail.ln` — deve fallire se `f` non viene consumato

---

### 2.3 Block Scoping per Variabili Non-Lineari — Testato?

Il test `block_scope_fail.ln` verifica che una variabile lineare (`mov x File`) dentro un `if` block causi un errore se non consumata. Ma:

1. Non c'è un test che verifica il **block scoping per variabili normali** (non-lineari):
   ```lain
   if true {
       var x = 10
   }
   // x qui è in scope o no?
   ```
2. Non c'è un test per il **for loop scoping**:
   ```lain
   for i in 0..10 { }
   // i qui è visibile o no?
   ```

---

### 2.4 Struct Construction — Manca la Validazione All-Fields-Initialized

La specifica (§3.6) mostra due modalità di costruzione struct:
```lain
var p = Point(10, 20)     // Positional
var p Point                // Field-by-field
p.x = 10
p.y = 20
```

Ma **cosa succede se un campo non viene inizializzato?**
```lain
var p Point
p.x = 10
// p.y non inizializzato — errore o undefined behavior?
```

La specifica non documenta se il compilatore controlla che tutti i campi siano inizializzati. Per un linguaggio safety-critical, questo è fondamentale.

---

### 2.5 Tipo di Ritorno di `main` — Opzionale o Obbligatorio?

I test mostrano sia:
```lain
proc main() int { return 0 }    // Con return type
proc main() { }                  // Senza return type (syntax_check.ln, unsafe_valid.ln)
```

La specifica (§6.4) mostra entrambe le forme ma non chiarisce se il tipo di ritorno è opzionale. C richiede `int main()`, Lain deve specificare cosa succede.

---

### 2.6 Nested Type `Token.Kind` — Semantica Non Chiara

Il progetto `Lain_1/src/main.ln` usa:
```lain
type Token.Kind { EOF, Error, ... }
type Token { kind Token.Kind, lexeme u8[] }
```

La specifica (§3.10) documenta questa feature, ma restano domande:
1. `Token.Kind` è un tipo completamente indipendente da `Token`, o è legato?
2. Si può avere `type Foo.Bar.Baz {}` (nesting multiplo)?
3. C'è accesso a `Token.Kind.EOF` o solo `Token.Kind.EOF`?
4. Nella C emission, come viene mangled?

---

### 2.7 Semantica delle Immutable Bindings per Struct — Controintuitiva

```lain
x = Point(10, 20)     // Immutable binding
x.y = 30              // Questo è permesso? La specifica non lo chiarisce
```

Se `x` è immutabile, i campi dovrebbero essere immutabili (deep immutability). Ma il compilatore probabilmente non enforcea questo, perché la semantica di `x = ...` è di basso livello (un assignment in C).

---

## 🟡 3. Problemi nei Test

---

### 3.1 `ownership.ln` Usa `printf` Senza Extern — Ancora Non Corretto

Come segnalato in analysis2 (§3.1), il test `tests/safety/ownership/ownership.ln` chiama `printf` senza dichiarazione `extern`:
```lain
func print_point(p Point) {
    printf("Point(%d, %d)\n", p.x, p.y)
}
```

Questo presto non funzionerà più in C23 dove implicit function declarations saranno un hard error.

**Fix**: Aggiungere `extern proc libc_printf(fmt *u8, ...) int` e usare `libc_printf`.

---

### 3.2 `purity_fail.ln` Ha la Violation 1 Commentata — Ancora Non Corretto

```lain
func pure_add(a int, b int) int {
    // impure_increment()        // ← ANCORA COMMENTATA
    global_counter = 100         // ← unica violazione attiva
    return a + b
}
```

Questa debolezza è mitigata dal nuovo test `repro_purity_fail.ln` che testa specificamente "func chiama proc". Tuttavia, `purity_fail.ln` dovrebbe essere corretto per testare entrambe le violazioni.

**Fix suggerito**: Decomporlo in due test separati:
- `purity_global_fail.ln` — func modifica globale
- `purity_call_proc_fail.ln` — func chiama proc (già coperto da `repro_purity_fail.ln`)

---

### 3.3 `test_extern.ln` — Ownership Violation con `malloc` — Ancora Non Corretto

```lain
extern func malloc(size usize) mov *void
var ptr *void = malloc(10)   // malloc ritorna mov *void, ma ptr è var *void
free(ptr)                     // free vuole mov *void, ma ptr non è mov
```

Due violazioni:
1. `malloc` ritorna un puntatore owned, ma non viene ricevuto come `mov`
2. `free(ptr)` non annota `mov ptr` al call-site

Il fatto che compili indica che il linearity checker non verifica `*void` tramite extern. Servono test negativi per questi casi.

---

### 3.4 `exhaustive_fail.ln` È Fuori dalla Cartella `tests/` — Misplaced

Il file `tests/exhaustive_fail.ln` è nella root di `tests/`, non in una sottocartella. Dovrebbe essere in `tests/types/exhaustive_fail.ln` per coerenza con il pattern organizzativo.

---

### 3.5 `syntax_check.ln` È Escluso dai Test — Perché?

In `run_tests.sh`:
```bash
find tests -name "*.ln" ! -name "*_fail*" ! -name "syntax_check.ln" | sort
```

`syntax_check.ln` è esplicitamente escluso. Questo test usa `return var`:
```lain
func ret_var() var int {
    var x = 10
    return var x
}
```

Se `return var` di una variabile locale è una dangling reference (come la specifica avverte in §5.7), allora questo test dovrebbe essere un `_fail` test, non un test escluso. L'esclusione nasconde un potenziale bug.

---

### 3.6 Test Coverage — Aree Mancanti (Aggiornamento da analysis2)

| Area | Stato | Note |
|:-----|:------|:-----|
| Slice operations (`arr[0..3]`) | ❌ Assente | Feature fantasma: documentata, probabilmente non implementata |
| Nested structs (struct in struct) | ❌ Assente | Nessun test su composizione non-lineare |
| ADT con dati lineari (`mov` in variant) | ❌ Assente | Sottodocumentato |
| Bitwise operators (`&`, `\|`, `^`, `~`) | ❌ Assente | Nessun test |
| Compound assignments (`+=`, `-=`, ecc.) | ❌ Assente | Nessun test |
| Char literals (`'A'`, `'\n'`) | ❌ Assente | Nessun test |
| `for i, val in 0..10` (two-var form) | ❌ Assente | Documentato in §7.2, mai testato |
| Shadowing negato | ❌ Assente | §4.6 lo vieta, nessun test verifica |
| Immutable globals in func | ❌ Assente | read di globale in func pura |
| Recursive data types | ❌ Assente | Come si definisce una linked list? |
| Multiple return paths con constraints | ⚠️ Parziale | Solo bounds constraints |
| Module namespace conflicts | ❌ Assente | Due moduli con stessa funzione? |
| **Block scope per var normali** | ❌ **Nuovo** | Nessun test per scope exit di non-lineari |
| **Uninit struct fields** | ❌ **Nuovo** | Struct parzialmente inizializzata |
| **`close_file` senza `mov`** | ❌ **Nuovo** | Test negativo mancante |
| **`return var` dangling** | ❌ **Nuovo** | `syntax_check.ln` è escluso, non _fail |
| **Float / assenza di float** | ❌ **Nuovo** | Non documentato in nessun modo |
| **`*void` cast** | ❌ **Nuovo** | Come convertire `*void` a `*int`? |

---

## 🔵 4. Lacune nella Specifica

Aspetti del linguaggio che **non sono documentati** o sono documentati in modo insufficiente.

---

### 4.1 Nessuna Sezione "Error Model" — Come Si Gestiscono gli Errori?

La specifica non ha una sezione su error handling. Domande senza risposta:
- Come si segnala un errore da una funzione (e.g., `open_file` fallisce)?
- Si usa un tipo `Result`/`Option`? Se no, come si distingue successo da fallimento?
- I codici di ritorno (`int` return) sono l'unico meccanismo?
- Le eccezioni sono escluse per design? (Se sì, va documentato)

Per un linguaggio embedded safety-critical, l'error model è **fondamentale**.

---

### 4.2 Nessuna Sezione "Memory Model" — Stack vs Heap

La specifica non chiarisce:
- Tutte le variabili locali sono stack-allocated?
- Le struct sono sempre by-value (stack)?
- Come si alloca heap senza `malloc` (che è `extern`)?
- Esiste un allocatore Lain-native?

Per un linguaggio che promette "zero runtime overhead" e "no GC", il memory model deve essere esplicito.

---

### 4.3 ABI / Calling Convention — Non Documentata

Come vengono passati i parametri nelle diverse ownership modes?
- Shared (`p T`): la specifica dice `const T*` — è per **qualsiasi** tipo o solo per struct?
- Mutable (`var p T`): dice `T*` — sempre per puntatore?
- Owned (`mov p T`): dice `T` by value — e se la struct è grande?
- I tipi primitivi (`int`, `u8`) sono passati per valore in tutti i modi?

La tabella in §5.1 è un buon inizio ma lascia molte domande aperte. E.g.:
```lain
func add(a int, b int) int    // a e b sono per valore o per const int*?
```

---

### 4.4 Stringhe Multi-Byte / Unicode — Non Documentato

`u8[:0]` è definito come "null-terminated byte slice". Ma:
- UTF-8 è supportato? 
- `.len` restituisce byte o codepoints?
- Un literal `"café"` ha `.len == 5` (byte) o `.len == 4` (chars)?

Per un linguaggio moderno, il trattamento di Unicode deve essere esplicito.

---

### 4.5 Overflow Aritmetico — La Specifica Lo Ammette Come Non Affrontato

La §13 dice: `Integer Overflow: *Not yet addressed*`. Questo va espanso:
- Wrapping (stile C/Rust wrapping_add)?
- Undefined behavior?
- Trap in debug mode?

Dato che Lain genera C99, l'overflow di signed int è UB in C. Questo va documentato come rischio.

---

### 4.6 Zero-Initialization — Le Variabili Non Inizializzate Che Valore Hanno?

```lain
var x int            // Che valore ha x? 0? Garbage?
var p Point          // I campi sono 0? Garbage?
var arr int[5]       // Zeri? Garbage?
```

La specifica non lo dice. In C, le variabili locali non sono inizializzate (garbage). Se Lain segue C, va documentato. Se Lain zero-inizializza, va documentato e implementato.

---

### 4.7 Commenti Nell'Grammar — Mancanti

L'Appendix C (pseudo-BNF) non include la produzione per i commenti:
```
comment = "//" { any_char } newline
        | "/*" { any_char } "*/" ;
```

---

### 4.8 Struct Positional Construction — Ordine dei Campi

```lain
type Point { x int, y int }
var p = Point(10, 20)     // 10 → x, 20 → y
```

La specifica mostra questa sintassi ma non specifica:
- L'ordine è **strettamente** l'ordine di dichiarazione?
- Cosa succede se il numero di argomenti non corrisponde?
- I named arguments sono supportati? (e.g., `Point(y: 20, x: 10)`)

---

### 4.9 ADT Variant Access Senza `case` — Non Documentato

Come si accede ai dati di un ADT variant senza pattern matching?
```lain
var s = Shape.Circle(10)
// Come ottengo radius senza case? s.radius? Errore?
```

La specifica mostra solo `case` per accedere ai dati. Va chiarito se è l'**unico** modo.

---

### 4.10 Regole di Visibilità dei Moduli — Public by Default?

La specifica (§10.1) dice:
> All public declarations from the imported module become available in the current scope.

Ma non definisce cosa rende una dichiarazione "public". Tutte le dichiarazioni sono public? Non esiste `private`? Non esiste `export`? (`export` è riservato ma non implementato).

---

## 🟣 5. Problemi nella Standard Library

---

### 5.1 `std/c.ln` — Purity Annotations Errate

Come discusso in §1.1, le funzioni I/O sono dichiarate `func` (pure):
```lain
extern func fopen(filename *u8, mode *u8) mov *FILE    // DOVREBBE: extern proc
extern func fclose(stream mov *FILE) int               // DOVREBBE: extern proc
extern func fputs(s *u8, stream *FILE) int             // DOVREBBE: extern proc
extern func fgets(s var *u8, n int, stream *FILE) var *u8  // DOVREBBE: extern proc
```

Solo `printf` e `libc_printf` sono coinvolte nel discorso purity dei test, ma **tutte** le funzioni I/O devono essere `proc`.

---

### 5.2 `std/io.ln` — `print` e `println` Sono `func` Ma Fanno I/O

```lain
func print(s *u8) {
    libc_printf("%s", s)
}
func println(s *u8) {
    libc_puts(s)
}
```

Entrambe fanno output a stdout — sono `proc`, non `func`. (Funzionano solo perché `libc_printf`/`libc_puts` sono dichiarate `extern func`.)

---

### 5.3 `std/io.ln` — Prendono `*u8` Anziché `u8[:0]`

Le funzioni `print`/`println` prendono `*u8` (raw pointer), non `u8[:0]` (sentinel slice). Questo:
1. Perde l'informazione di lunghezza
2. Non è type-safe rispetto al sistema Lain
3. Non corrisponde a come gli string literal sono tipizzati (`u8[:0]`)

L'utente deve fare `print(mystring.data)` anziché `print(mystring)`. Va corretto.

---

### 5.4 `std/fs.ln` — `write_file` Prende `var f File` Ma Ha Bisogno Solo di Read

```lain
proc write_file(var f File, s u8[:0]) {
    fputs(s.data, f.handle)
}
```

`write_file` prende `var f File` (mutable borrow) ma non modifica `f` — legge solo `f.handle`. Dovrebbe prendere `f File` (shared borrow). L'uso di `var` qui è un anti-pattern che viola il principio di minimo privilegio.

---

## 📋 6. Roadmap Prioritizzata

### Fase A: Correzione Purity nella Stdlib (CRITICA)

| # | Task | Tipo | Impatto |
|---|------|------|---------|
| A.1 | `std/c.ln`: fopen, fclose, fputs, fgets → `extern proc` | Codice | 🔴 Correttezza |
| A.2 | `std/fs.ln`: `open_file` → `proc` | Codice | 🔴 Coerenza |
| A.3 | `std/io.ln`: print, println → `proc` | Codice | 🔴 Coerenza |
| A.4 | `std/io.ln`: parametri `*u8` → `u8[:0]` | Codice | 🟠 Type-safety |
| A.5 | `std/fs.ln`: `write_file` → shared borrow `f File` | Codice | 🟡 Best practice |
| A.6 | Aggiornare `specification.md` §10.2 con le nuove dichiarazioni | Spec | 🔴 Coerenza |

> [!WARNING]
> Cambiare le extern a `proc` in `std/c.ln` **romperà tutti i test** che usano `func main()` con I/O. Prima di applicare A.1, tutti i test devono essere verificati per coerenza `proc main()`.

---

### Fase B: Correzione Test e Coverage

| # | Task | Tipo | Effort |
|---|------|------|--------|
| B.1 | Fix `ownership.ln`: aggiungere `extern proc libc_printf` | Test fix | Basso |
| B.2 | Fix `purity_fail.ln`: decomporlo in 2 test separati | Test fix | Basso |
| B.3 | Fix `test_extern.ln`: annotare `mov` su `malloc`/`free` call sites | Test fix | Basso |
| B.4 | Spostare `exhaustive_fail.ln` in `tests/types/` | Riorg | Basso |
| B.5 | Convertire `syntax_check.ln` in `return_var_dangling_fail.ln` oppure documentare perché è escluso | Test fix | Medio |
| B.6 | Aggiungere `shadowing_fail.ln` | Nuovo test | Basso |
| B.7 | Aggiungere test per bitwise operators | Nuovo test | Basso |
| B.8 | Aggiungere test per compound assignments | Nuovo test | Basso |
| B.9 | Aggiungere test per char literals | Nuovo test | Basso |
| B.10 | Aggiungere test per `for i, val in 0..10` | Nuovo test | Basso |
| B.11 | Aggiungere `close_file_no_mov_fail.ln` | Nuovo test | Medio |
| B.12 | Aggiungere test per block scope di variabili normali | Nuovo test | Medio |
| B.13 | Aggiungere test per struct uninit fields | Nuovo test | Medio |

---

### Fase C: Espansione della Specifica

| # | Task | Sezione |
|---|------|---------|
| C.1 | Aggiungere sezione "Error Model" | Nuova §15 |
| C.2 | Aggiungere sezione "Memory Model" (stack/heap distinction) | Nuova §16 |
| C.3 | Chiarire float: "Non supportati. Solo interi" oppure aggiungere `f32`/`f64` | §3.1 |
| C.4 | Documentare zero-initialization policy | §4 |
| C.5 | Documentare ABI / calling convention di parametri | §5.1 |
| C.6 | Documentare Unicode / multi-byte string handling | §3.5 |
| C.7 | Documentare overflow aritmetico (UB policy o wrapping) | §8.1 / §13 |
| C.8 | Documentare `*void` cast semantics e null checking | §3.9 |
| C.9 | Documentare visibility/export model per moduli | §10 |
| C.10 | Chiarire struct partial initialization behavior | §3.6 |
| C.11 | Documentare ADT variant access (solo via `case`?) | §3.7 |
| C.12 | Documentare nested types semantics (multi-level, C emission) | §3.10 |
| C.13 | Validare/implementare slice operations `arr[0..3]` | §3.4 |

---

### Fase D: Evoluzione del Linguaggio

| # | Task | Effort | Priorità |
|---|------|--------|----------|
| D.1 | Implementare tipo `Option` / `Result` per error handling | Alto | 🔴 Alta |
| D.2 | Completare bounce check su accessi dinamici (§9.7 widening) | Alto | 🟠 Media |
| D.3 | Implementare `for elem in array` (iteration over arrays) | Medio | 🟡 Media |
| D.4 | Implementare float types (`f32`, `f64`) se desiderato | Medio | 🟡 Bassa |
| D.5 | Eliminare whitelist hardcoded per type mapping C nell'emitter | Alto | 🟠 Media |
| D.6 | Comptime metaprogramming (generics via CTFE) | Molto Alto | 🔵 Futura |
| D.7 | RAII / Destructori impliciti (`drop` trait) | Alto | 🔵 Futura |
| D.8 | Trait / Interfacce | Alto | 🔵 Futura |

---

## Appendice: Stato Rispetto ad analysis2

| Item analysis2 | Stato | Note |
|:----------------|:------|:-----|
| 1.1 `match` vs `case` | ✅ Risolto | Specifica usa `case` ovunque |
| 1.2 `bool` non documentato | ✅ Risolto | §3.1 documenta `bool`, `true`, `false` |
| 1.3 `as` documentato "reserved" | ✅ Risolto | §8.6 documenta `as` come implementato |
| 1.4 Tipi interi non documentati | ✅ Risolto | §3.1 tabella completa con tutti i tipi |
| 1.5 `while` in `func` | ✅ Risolto | `while_loop.ln` ora usa `proc main()` + `while_in_func_fail.ln` |
| 1.6 `func main()` nei test | ✅ Parziale | Molti test corretti, ma `borrow_pass.ln` e altri ancora usano `func main()` senza I/O → OK se non chiamano proc |
| 1.7 `extern func` vs `extern proc` | ✅ Documentato ma non risolto | §6.5 descrive la semantica, ma `std/c.ln` è ancora errato |
| 2.1 Scope piatto | ✅ Risolto | Block scoping implementato, test presente |
| 2.2 Bounds dinamici | ⏳ Non risolto | Ancora da implementare (§9.7 limitation) |
| 2.3 ADT con mov | ⏳ Non testato | Nessun miglioramento |
| 2.4 `libc_printf` hack | ✅ Documentato | §14.5 documenta il meccanismo `-D` |
| 2.5 Address-of `&` | ✅ Risolto | §12.4 documenta `&` come unsafe-only |
| 2.6 `return var` | ✅ Documentato | §5.7 documenta la semantica |
| 2.7 Binding disambiguation | ✅ Documentato | §4.5 documenta la regola |
| PF2.1 Import non preserva `mov` | ⚠️ Stato incerto | Va verificato con test specifici |
| PF2.3 `close_file` bug linearità | ✅ Risolto | Usa destructuring `mov {handle} File` |
| PF2.4 Test con `func main()` + I/O | ✅ Parziale | Molti test corretti a `proc main()` |
| PF2.5 Linearity errors senza line/col | ⚠️ Stato incerto | Va verificato |

---

## Conclusione

La specifica ha fatto un **salto di qualità notevole** dall'analysis2: è passata da ~800 righe inaffidabili a 1643 righe accuratamente organizzate. La maggior parte delle divergenze critiche spec↔implementazione è stata risolta.

I problemi rimanenti si dividono in tre categorie:

1. **Purity della stdlib** (Fase A): Il problema più urgente. Le funzioni I/O in `std/c.ln` sono dichiarate `func` (pure) quando dovrebbero essere `proc`. Questo mina la garanzia fondamentale del linguaggio. La correzione è meccanica ma impatta tutti i test.

2. **Lacune nella specifica** (Fase C): Mancano intere sezioni su error model, memory model, float, Unicode, zero-initialization, ABI. Queste non sono bug ma aree dove la specifica **non dice nulla**, lasciando comportamenti ambigui o undefined.

3. **Test coverage** (Fase B): Diverse aree del linguaggio non hanno test (bitwise, compound assignments, char literals, slice operations, shadowing). I test esistenti hanno alcuni problemi noti (`ownership.ln` con printf bare, `test_extern.ln` con ownership violation).

La priorità assoluta è la **Fase A** (purity), perché la correttezza del type system è il valore fondamentale di Lain.
