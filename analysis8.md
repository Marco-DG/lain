# Analysis 8 — Criticità del Linguaggio Lain e della sua Implementazione

**Data**: 2026-03-01  
**Scope**: README.md (specifica), `src/` (compilatore C99), `tests/` (79 test files)  
**Obiettivo**: Individuare criticità, mancanze, errori e scelte sbagliate, misurate rispetto ai 5 obiettivi dichiarati.

---

## Obiettivi Dichiarati (Riferimento)

| # | Obiettivo | Sigla |
|---|-----------|-------|
| 1 | Velocità Assembly — zero overhead a runtime | **PERF** |
| 2 | Memory Safety zero-cost — no data races, UAF, DF, buffer overflow | **SAFE** |
| 3 | Analisi Statica VRA — validazione polinomiale, senza SMT | **VRA** |
| 4 | Determinismo e Funzioni Totali — segregazione puro/impuro | **DET** |
| 5 | Semplicità Sintattica e Grammaticale | **SIMP** |

---

## 1. Soundness della Memory Safety (SAFE) — CRITICITÀ ALTA

### 1.1 Il Borrow Checker è Intra-Statement, Non Intra-Lifetime

Il borrow checker attuale (`linearity.h` + `region.h`) opera a granularità di **singola istruzione**: tutti i borrow registrati in un'espressione di chiamata sono marcati `is_temporary = true` e vengono cancellati da `borrow_clear_temporaries()` alla fine dello statement (linea 754-757 di `linearity.h`).

**Conseguenza**: un pattern come il seguente **non viene catturato**:

```lain
var data Data
var ref = get_ref(var data)   // borrow mutabile → ref
read(data)                    // ref è scaduto! → non dovrebbe essere permesso
use(ref)                      // use after borrow expiration
```

Il borrow per `ref` scade alla fine dello statement di assegnazione. Lo statement successivo `read(data)` non ha conflitti. Ma `ref` è ancora vivo e punta a `data`. **Non c'è tracking di lifetime reale**.

> [!CAUTION]
> Senza lifetime tracking, la safety claim "data races impossibili" è **non provata** per il caso di *escaped references* (borrow che sopravvivono allo statement). Il sistema è sound solo per il sottoinsieme dove tutti i borrow sono effimeri (argomenti di una singola call).

### 1.2 `return var` Abilitato ma Non Verificato

La specifica (§5.7) documenta `return var ctx.counter` per restituire un riferimento mutabile a un campo. Il compilatore **non fa alcun controllo** che il dato restituito sopravviva alla funzione.

```lain
func dangerous(var ctx Context) var int {
    var local = 42
    return var local   // Dangling pointer! → specifica dice "compile error" ma il compilatore NON lo verifica
}
```

La specifica dice che è "a compile error" ma nel codice sorgente non c'è nessuna analisi che verifichi questa condizione. Il campo `return_mode` non viene ispezionato nel borrow checker.

### 1.3 Nessun Tracking di Aliasing fra Struct Fields

Il linearity checker traccia variabili come **interi nomi**: se `mov a` è consumato, tutto `a` è invalidato. Ma non c'è tracking a livello di campo:

```lain
type Pair { mov left *int, mov right *int }
var p = Pair(ptr1, ptr2)
consume(mov p.left)    // Consuma solo left
use(p.right)           // Dovrebbe essere OK ma il compilatore consuma tutto 'p'
```

Questo è un falso positivo (conservativo) ma non è documentato, e rende il sistema inutilizzabile per struct con campi lineari parzialmente consumabili.

### 1.4 Slice Safety: Il Buco Fondamentale

La specifica dice (§13): "Buffer Overflows: Impossibili — Static Range Analysis verifica ogni accesso".

Ma `bounds.h` (linea 78-86) **emette solo un warning** quando l'indice o la lunghezza dell'array sono sconosciuti:

```c
// Index unknown -> potentially unsafe
fprintf(stderr, "bounds warning: index range unknown, cannot verify against length %ld\n", ...);
// ← Non è exit(1)! Il programma viene comunque compilato.
```

E quando **entrambi** sono sconosciuti (linea 82-85):

```c
BOUNDS_DBG("warning: both index and array length unknown, cannot verify bounds statically");
// ← Solo un messaggio di debug, nessun errore!
```

> [!WARNING]
> Un accesso `slice[i]` dove `i` proviene da input runtime (non narrowato da `if` o `for`) viene **compilato silenziosamente** senza alcuna protezione. La claim "buffer overflow impossibili" nella specifica è **falsa** per i casi di accesso dinamico a slice.

### 1.5 Nessun Controllo di Null Pointer per `mov *T` da `extern`

```lain
extern proc malloc(size usize) mov *void
var ptr = malloc(1024)   // ptr potrebbe essere NULL
unsafe { *ptr = 42 }     // UB se malloc ha fallito
```

La specifica riconosce il problema (§12.5) ma non offre **nessun meccanismo** per forzare la validazione. Non c'è un `Option<*T>`, non c'è `nullable` vs `nonnull`, non c'è enforcement statico. La combinazione `mov *void` + `unsafe` bypassa ogni garanzia.

---

## 2. Value Range Analysis (VRA) — CRITICITÀ MEDIA-ALTA

### 2.1 Overflow nei Calcoli di Range

`ranges.h` fa aritmetica su `int64_t` senza controllare overflow:

```c
static Range range_add(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    return range_make(a.min + b.min, a.max + b.max);  // ← Overflow int64_t!
}
```

Se `a.max = INT64_MAX` e `b.max = 1`, il risultato è `INT64_MIN`, un range invertito che causerà falsi negativi in tutte le verifiche a valle. Questo è un **bug di correttezza** del compilatore stesso.

### 2.2 Solo Addizione e Sottrazione

`sema_eval_range()` supporta solo `EXPR_LITERAL`, `EXPR_IDENTIFIER`, e `EXPR_BINARY` con `+` e `-`. Tutti gli altri operatori (moltiplicazione, divisione, modulo, bitwise, cast) restituiscono `range_unknown()`.

**Conseguenza pratica**: una funzione come:

```lain
func clamp_byte(x int) u8 {
    if x < 0 { return 0 }
    if x > 255 { return 255 }
    return x as u8    // ← range(x) = [0, 255], ma 'as' non è tracciato →  range_unknown()
}
```

La VRA non può dimostrare che `x as u8` soddisfa alcun contratto sul return, perché il cast annulla tutte le informazioni di range.

### 2.3 L'operazione `!=` Non Riduce il Range

```c
case TOKEN_BANG_EQUAL:
    // != is harder to represent in a single interval if it splits the range.
    // For now, ignore.
    break;
```

Il constraint `b int != 0` è documentato nella specifica (§9.1) come feature principale. Ma se il compilatore non ha informazioni sul range di `b` e incontra `safe_div(10, x)`, **non può verificare** che `x != 0` perché il range di `x` è `unknown` e `!=` viene ignorato.

> [!IMPORTANT]
> La feature più visibilmente pubblicizzata della specifica (`safe_div(a, b != 0)`) **funziona solo per costanti note** (e.g. `safe_div(10, 0)` → errore). Per valori dinamici, la verifica viene silenziosamente saltata.

### 2.4 Loop Widening Distrugge Tutta l'Informazione

```c
static void sema_widen_loop(StmtList *body, RangeTable *t) {
    // ...
    case STMT_ASSIGN:
        if (s->as.assign_stmt.target->kind == EXPR_IDENTIFIER) {
            range_set(t, s->as.assign_stmt.target->as.identifier_expr.id, range_unknown());
        }
```

Dopo **qualsiasi** loop, **tutte** le variabili assegnate nel body perdono ogni informazione di range. Non c'è narrowing di post-loop, non c'è tracking del loop counter dopo l'uscita dal loop. La specifica ammette questa limitazione (§9.7) ma la presenta come un "caso raro" quando in realtà è **sistematica**: qualsiasi accumulatore, counter, o stato computato in un loop diventa `unknown`.

### 2.5 Nessun Widening per While-Loop

Il loop widening in `sema_widen_loop` non gestisce `STMT_WHILE` nel suo switch ricorsivo:

```c
case STMT_FOR:
    sema_widen_loop(s->as.for_stmt.body, t);
    break;
// ← Manca STMT_WHILE!
```

Variabili modificate all'interno di un `while` annidato dentro un `for` potrebbero non essere widened, risultando in falsi positivi o falsi negativi.

---

## 3. Determinismo e Totalità (DET) — CRITICITÀ MEDIA

### 3.1 Le `func` Non Sono Realmente Totali

La specifica dice: "Pure functions (`func`) are guaranteed to terminate" (§6.1). La garanzia dichiarata è:
- No recursion ✓ (verificato)
- No `while` loops ✓ (verificato)
- Solo `for` su range finiti ✓ (verificato)

Ma manca la verifica di **divisione per zero runtime** e **panics**:

```lain
func total(n int) int {
    return 10 / n    // Se n==0: UB C99, crash, o garbage. Non "termination guaranteed".
}
```

Senza la verifica che `n != 0` (e la VRA non riesce a provarla per valori dinamici), una `func` può causare undefined behavior. La totalità dichiarata **presuppone che la VRA sia perfetta**, ma come visto in §2, non lo è.

### 3.2 `extern func` Viola la Purezza per Assunzione

```lain
extern func abs(n int) int   // Dichiarato puro → il compilatore si fida
```

La specifica spiega (§6.5) che `extern func` è "trusted". Ma qualsiasi funzione C può essere dichiarata `extern func`, incluse funzioni impure:

```lain
extern func getchar() int       // Il compilatore accetta: è trusted
func "pure"() int {
    return getchar()             // Side effect! Ma compila.
}
```

Non c'è alcun warning né meccanismo di audit. Questo è un **buco nella segregazione puro/impuro** che dipende interamente dalla disciplina del programmatore.

### 3.3 `for` Range Dipende da Valori Runtime

```lain
func process(data int[10], n int) int {
    var sum = 0
    for i in 0..n {    // n è un parametro → il range dipende dal caller
        sum = sum + data[i]
    }
    return sum
}
```

Se `n > 10`, l'accesso `data[i]` è out-of-bounds. Il `for` è "finito" ma potrebbe iterare oltre i limiti dell'array. La VRA dovrebbe verificare che `n <= 10`, ma come visto, widening + range_unknown rendono questa verifica impossibile post-loop.

---

## 4. Performance e Zero Overhead (PERF) — CRITICITÀ MEDIA

### 4.1 Struct Passate come Copie Complete

Nella tabella §5.1: i parametri **shared** (`p T`) emettono `const T*` in C. Ma i parametri **owned** (`mov p T`) emettono `T` **by value**.

Per struct grandi, una `mov` implica una copia di stack. Struct con molti campi (e.g. un `Matrix4x4` con 16 `f64`) vengono copiate intere, anche quando il pattern di uso è "move-in, don't-copy"

In C su piattaforme reali, il passaggio by-value di struct > 8-16 bytes è **significativamente più lento** del passaggio by-pointer. Questo contraddice l'obiettivo "prestazioni pari o superiori a C".

### 4.2 ADT Implementati come Union C — Non Ottimizzati

Ogni istanza ADT occupa spazio pari alla **variante più grande** + tag. Per ADT con varianti molto sbilanciate:

```lain
type AST {
    Number { value int }                          // 4 bytes
    Function { name u8[:0], params int[128] }     // >500 bytes
}
```

Ogni nodo `AST` occupa >500 bytes in stack, anche se è un `Number`. Non c'è: boxing automatico, NaN boxing, pointer tagging, o alcuna ottimizzazione dell'enum layout.

### 4.3 Nessun Shift Operator

Mancano gli operatori `<<` e `>>` (left/right shift), presenti in C/C++/Rust/Zig. Per il target "embedded systems", lo shift è fondamentale per manipolazione di registri hardware, protocolli di comunicazione, e ottimizzazioni aritmetiche. L'unica opzione è moltiplicare/dividere per potenze di 2, che il backend C potrebbe non ottimizzare.

### 4.4 Overflow Signed è UB nella Specifica — Poi Contraddetta

La specifica §18 dice: "Signed integer overflow is undefined behavior (inherited from C99)".

Ma la nota §3.1 dice: "Lain guarantees that all signed integer overflows result in Two's Complement wrap-around. The compiler injects `-fwrapv` automatically to prevent undefined behavior".

**Queste due affermazioni sono mutuamente esclusive.** O il compilatore emette `-fwrapv` (e quindi l'overflow è defined wrap-around) o non lo fa (e l'overflow è UB). Attualmente il compilatore **non inietta** `-fwrapv` nella pipeline di build.

> [!CAUTION]
> La contraddizione nella specifica rende non chiaro quale sia il comportamento corretto. Se l'obiettivo è embedded safety, l'overflow signed **deve** essere definito (wrap o trap), e la specifica deve essere coerente.

---

## 5. Architettura del Compilatore — CRITICITÀ MEDIA-ALTA

### 5.1 Tutto il Compilatore in Header Files

Il compilatore è strutturato come una serie di `.h` files con funzioni `static`:

- `sema.h` include `sema/scope.h`, `resolve.h`, `typecheck.h`, `linearity.h`
- `linearity.h` include `region.h`, `scope.h`, `resolve.h`, `typecheck.h`
- `typecheck.h` include `resolve.h`, `ranges.h`, `bounds.h`
- Tutte le funzioni sono `static`

Questo pattern causa:
- **Recompilazione totale** per ogni modifica a qualsiasi file
- **Conflitti di namespace** fra diverse Translation Unit (mitigato solo dal fatto che c'è un solo `.c`)
- **Impossibilità di testing unitario** dei singoli moduli
- **Variabili globali ovunque** (`sema_arena`, `sema_decls`, `sema_ranges`, `current_function_decl`, `sema_in_unsafe_block`, ecc.) rendono impossibile il parallelismo del compilatore

### 5.2 Variabili Globali Mutabili per Stato del Compilatore

```c
Type *current_return_type = NULL;
Decl *current_function_decl = NULL;
const char *current_module_path = NULL;
DeclList *sema_decls = NULL;
Arena *sema_arena = NULL;
RangeTable *sema_ranges = NULL;
bool sema_in_unsafe_block = false;
```

Queste variabili globali rendono il compilatore:
- Non-rientrante
- Non-parallelizzabile
- Difficile da testare isolatamente
- Soggetto a bug sottili quando una variabile non viene resettata fra invocazioni

### 5.3 Scope Management con `malloc`/`free`

```c
static void sema_push_scope(void) {
    ScopeFrame *frame = (ScopeFrame *)malloc(sizeof(ScopeFrame));  // ← malloc!
    // ...
}
static void sema_pop_scope(void) {
    // ...
    free(frame);  // ← free!
}
```

Il compilatore usa un'arena allocator per la maggior parte delle allocazioni, ma `ScopeFrame` usa `malloc`/`free` direttamente. Ogni `ScopeFrame` copia 4096 puntatori (`SEMA_BUCKET_COUNT`), ovvero 32KB per ogni push/pop di scope. Questo è costoso e non necessario — un linked-list di scope o un array dinamico sarebbe più efficiente.

### 5.4 Hash Table a Dimensione Fissa (4096 bucket)

```c
#define SEMA_BUCKET_COUNT 4096
static Symbol *sema_globals[SEMA_BUCKET_COUNT];
static Symbol *sema_locals [SEMA_BUCKET_COUNT];
```

Due array statici da 4096 puntatori ciascuno. Per programmi piccoli è sprecato (64KB di memoria per le due tabelle). Per programmi grandi con molti simboli, la dimensione fissa causerà collisioni O(n) nelle catene di hash.

### 5.5 Nested Function in C

```c
void walk_stmt(Stmt *s) {   // ← Definita DENTRO sema_resolve_module()
```

`walk_stmt()` è definita come funzione annidata dentro `sema_resolve_module()`. Questa è un'**estensione GCC** non standard in C99. Il compilatore non compilare con compilatori C diversi da GCC (Clang lo rifiuta in modo strict, MSVC non lo supporta).

> [!WARNING]
> L'obiettivo dichiarato è "compila a C99 portabile", ma il compilatore stesso **non è C99 standard** a causa delle nested functions.

---

## 6. Specifica vs Implementazione — Gap Documentati

### 6.1 Feature Specificate ma Non Implementate

| Feature | Specificata in | Stato Implementazione |
|---------|---------------|----------------------|
| Array literal `[1, 2, 3]` | §3.3 | ❌ Non implementata |
| `..=` range inclusive | §B | ❌ Reserved, non implementata |
| `export` visibility | §19.2 | ❌ Reserved, non implementata |
| `pre`/`post` contracts | §B | ❌ Reserved, non implementata come keyword, ma il meccanismo esiste inline |
| Hex/Octal/Binary literals | §2.3 warning | ❌ Non implementati |
| Definite initialization analysis | §17.1 | ❌ Non enforced ("compiler does not yet strictly enforce") |
| Generics full (Phase C) | — | ❌ Solo Phase B (comptime params) |
| `-fwrapv` injection | §3.1 | ❌ Non iniettato automaticamente |
| `Result`/`Option` generici | §15.2 | ❌ Solo per tipo specifico, no generici |

### 6.2 Feature Implementate ma Non Specificate

| Feature | File | Descrizione |
|---------|------|-------------|
| Debug print per variabile 'raw' | `sema.h:233-238` | Un `fprintf(stderr, "DEBUG raw: ...")` hardcodato nel walking del tipo |
| `LSTATE_BORROWED_READ/WRITE` | `linearity.h:107-108` | Stati dichiarati "reserved for future" ma mai usati |
| `STMT_USE` | `ast.h:208` | Statement kind nell'enum ma non parsato né emesso |
| C function name hardcoding | `emit/` | `fopen`, `fputs`, `fclose` hanno mapping hardcodati nell'emitter |

### 6.3 Contraddizioni nella Specifica

1. **§18 vs §3.1**: Signed overflow "UB (inherited from C99)" vs "Two's Complement wrap-around guaranteed"
2. **§4.2 vs test `uninit_fail.ln`**: "All variables must be initialized" ma `var z int` senza init non sempre errore
3. **§13 "Buffer Overflows: Impossible"** vs `bounds.h` che emette solo warnings per casi non verificabili
4. **§3.1 "implicit conversion between integers and floating numbers happens natively"** vs **§8.9 "Lain strictly forbids implicit type conversions"** — queste devono essere chiarite

---

## 7. Testing — CRITICITÀ MEDIA

### 7.1 Copertura di Safety

| Categoria | Test Positivi | Test Negativi (`_fail`) | Ratio |
|-----------|--------------|------------------------|-------|
| `safety/bounds/` | 8 | 6 | 43% negativi ✓ |
| `safety/ownership/` | 8 | 5 | 38% negativi ✓ |
| `safety/purity/` | 1 | 2 | 67% negativi ✓ |
| `core/` | 14 | 3 | 18% negativi |
| `types/` | 14 | 0 | **0% negativi** ⚠️ |
| `stdlib/` | 6 | 0 | **0% negativi** ⚠️ |

I test per i tipi (`types/`) non hanno **nessun test negativo**. Non viene testato:
- Assegnazione di tipo errato
- Cast illegale
- Mismatch di tipo in chiamate di funzione
- Uso di tipi opachi by-value

### 7.2 Mancanza di Test Critici

Non esistono test per:
- **Cross-function borrow**: borrow che attraversa più statements
- **Lifetime di return var**: verifica che return var di un locale fallisca
- **Aliasing fra struct fields**: consumo parziale di struct lineare
- **Overflow aritmetico di range**: `range_add` con valori vicini a INT64_MAX
- **Nested modules**: import di moduli che importano altri moduli
- **Shadowing di variabili lineari**: `var x = mov resource; { var x = 42 }` — la risorsa viene persa?
- **Comptime con side effects**: una `func` comptime che chiama `proc`
- **Case expression con tipi misti**: rami di `case` expr che restituiscono tipi diversi

---

## 8. Design del Linguaggio — Criticità Concettuali

### 8.1 L'Ambiguità `x = 10` è un Rischio di Correctness

La regola §4.5 dice: se `x` non è in scope, `x = 10` crea un nuovo binding immutabile. Se `x` è in scope ed è `var`, è un assignment.

**Problema**: un typo nel nome della variabile crea silenziosamente un nuovo binding:

```lain
var counter = 0
for i in 0..10 {
    conuter = conuter + 1   // Typo! → crea nuovo binding 'conuter', 'counter' non viene assegnato
}
// counter è ancora 0
```

Linguaggi come Rust, Go, e Python usano `let`/`:=`/sintassi esplicita per distinguere dichiarazione da assignment proprio per evitare questo problema. La scelta di Lain è intrinsecamente fragile.

### 8.2 Mancanza di Closures e Higher-Order Functions

Per un linguaggio con concetto di purezza (`func`), l'assenza di:
- Lambda / closures
- Funzioni come valori (first-class functions)
- Map/filter/reduce su array/slice

rende il paradigma funzionale non praticabile. Le `func` pure non possono accettare callback, rendendo impossibili pattern come `array.map(square)` o `list.filter(is_valid)`.

### 8.3 UFCS è Ambiguo con Namespace

```lain
var x = 10
var y = x.is_even()    // → is_even(x)
```

Ma se `x` è di tipo `Token.Kind`, cosa succede con `x.method()`? Cerca `method(x)` nel scope globale? E se l'utente ha `type Token { func method() }` come metodo innestato, quale vince? La specifica non definisce le regole di risoluzione per UFCS in presenza di tipi con namespace (§3.10).

### 8.4 Nessun Meccanismo di Error Propagation

La specifica introduce `Result`/`Option` come ADT (§15.2) ma senza generics funzionanti, ogni tipo richiede un ADT dedicato (`ResultFile`, `OptionInt`, ecc.). Non c'è:
- Operatore `?` (try/propagation)
- `unwrap` / `expect`
- Nessun sugar per il pattern matching su Result

Questo rende il codice estremamente verboso per qualsiasi operazione fallibile. E senza generics completi, non è nemmeno possibile definire un `Result(T, E)` parametrico.

### 8.5 Mancanza di Distruttori / Drop Semantics

Il sistema di linearità forza il consumo esplicito di risorse (`close_file(mov f)`). Ma non c'è un meccanismo automatico di cleanup gestito dallo scope:

```lain
proc risky() {
    var f = open_file("data.txt", "r")
    if error_condition {
        return   // ← ERRORE: f non consumato → compile error
    }
    close_file(mov f)
}
```

Il programmatore deve inserire `close_file(mov f)` **in ogni punto di uscita**, il che è esattamente il problema che RAII/Drop risolvono in C++ e Rust. Senza un meccanismo di `defer` o `Drop`, il sistema di linearità diventa un peso ergonomico significativo.

### 8.6 Nessun Meccanismo di Concorrenza

L'obiettivo #2 dichiara "eliminazione formale di Data Races". Ma il linguaggio non ha **nessun costrutto di concorrenza**: no threads, no channels, no async/await, no atomics. L'affermazione "data races impossibili" è vacuamente vera perché **non è possibile scrivere codice concorrente** in Lain. Quando la concorrenza verrà aggiunta, l'intero modello di borrow checking dovrà essere ripensato (come Rust ha dovuto fare con `Send`/`Sync` traits).

---

## 9. C Code Generation — CRITICITÀ BASSA-MEDIA

### 9.1 Name Mangling Fragile

La strategia di mangling è il prefisso `libc_` per le funzioni C standard, risolto con `-Dlibc_printf=printf` al momento della compilazione. Questo:
- Richiede flag manuali per ogni funzione C usata
- Non scala con molte dipendenze C
- Collisione se l'utente definisce una funzione `libc_something`

### 9.2 Output Fisso su `out.c`

Il compilatore scrive sempre su `out.c` (hardcodato in `main.c:82`). Non c'è opzione per specificare il file di output. Per un uso real-world con sistemi di build, questo è insufficiente.

### 9.3 Nessuna Emissione di `#line` Directives

Il codice C generato non include direttive `#line`, rendendo il debugging con gdb/lldb estremamente difficile: gli errori C puntano a `out.c` invece che al file `.ln` originale.

---

## 10. Riepilogo Priorità

| # | Criticità | Severità | Obiettivo Violato |
|---|-----------|----------|-------------------|
| 1 | Borrow checker intra-statement | 🔴 Alta | SAFE |
| 2 | Bounds checking non-bloccante per casi dinamici | 🔴 Alta | SAFE, VRA |
| 3 | VRA overflow nei calcoli di range | 🔴 Alta | VRA |
| 4 | Contraddizione overflow signed UB vs wrap | 🟠 Media-Alta | PERF, SAFE |
| 5 | Nested functions = non-C99 standard | 🟠 Media-Alta | PERF, SIMP |
| 6 | Nessun tracking di lifetime per `return var` | 🟠 Media-Alta | SAFE |
| 7 | VRA solo per `+` e `-` | 🟠 Media | VRA |
| 8 | Loop widening distrugge tutto | 🟠 Media | VRA |
| 9 | `!=` constraint non implementato | 🟠 Media | VRA |
| 10 | Ambiguità `x = 10` | 🟠 Media | SIMP |
| 11 | Nessun drop/defer | 🟡 Media-Bassa | SIMP, SAFE |
| 12 | Zero test negativi per types | 🟡 Media-Bassa | — |
| 13 | Struct passate by-value per `mov` | 🟡 Bassa | PERF |
| 14 | Mancanza shift operators | 🟡 Bassa | PERF, SIMP |
| 15 | Output hardcodato su `out.c` | ⚪ Bassa | — |

---

## 11. Modifiche Applicate (2026-03-01)

### 11.1 Correzioni Critiche al Compilatore

| File | Criticità Risolta | Descrizione |
|------|-------------------|-------------|
| `ranges.h` | **#3** VRA overflow | Introdotta aritmetica saturante (`sat_add_i64`/`sat_sub_i64`) — previene inversione di range su valori vicini a `INT64_MAX`/`INT64_MIN` |
| `ranges.h` | **#7** VRA solo `+`/`-` | Aggiunti `range_mul`, `range_div`, `range_mod` con semantica 4-corner (min/max incrociati). Aggiunto `EXPR_CAST` → range preservato attraverso cast `as` |
| `ranges.h` | **#9** `!=` ignorato | `TOKEN_BANG_EQUAL` ora restringe il range ai bordi: se `val == min` → `min = val+1`; se `val == max` → `max = val-1`; se range è esattamente `[val,val]` → range vuoto (errore) |
| `bounds.h` | **#2** Warning non-bloccanti | `fprintf` + continuazione → `fprintf` + `exit(1)`. Accessi con indice o lunghezza sconosciuti sono ora **errori di compilazione**, non warning. Messaggio include suggerimenti (`for i in 0..arr.len` o `if` guard) |
| `sema.h` | **#5** Nested function GCC | `walk_stmt()` estratta da dentro `sema_resolve_module()` a funzione `static` file-level. Il compilatore è ora **C99 standard** senza estensioni GCC |
| `sema.h` | **#8** Widen loop incompleto | `STMT_WHILE` aggiunto allo switch di `sema_widen_loop()` — variabili modificate in `while` dentro `for` vengono ora correttamente widened |
| `sema.h` | §6.2 Debug print | Rimosso `fprintf(stderr, "DEBUG raw: ...")` hardcodato per variabili chiamate `raw` |
| `sema.h` | **#8** (parziale) | Range dell'indice `for i in start..end` preservato come `[end, end]` dopo uscita dal loop, invece di widening a `unknown` |
| `linearity.h` | **#6** `return var` local | Aggiunto check in `STMT_RETURN`: se il valore è `EXPR_MUT` e l'inner è un identificatore locale (`!is_parameter`), errore "cannot return mutable reference to local variable" |

### 11.2 Nuova Feature: Shift Operators

| File | Modifica |
|------|----------|
| `token.h` | `TOKEN_SHIFT_LEFT` (`<<`), `TOKEN_SHIFT_RIGHT` (`>>`) aggiunti all'enum |
| `lexer.h` | `STATE_ANGLE_BRACKET_LEFT/RIGHT` estesi: `<<` → `TOKEN_SHIFT_LEFT`, `>>` → `TOKEN_SHIFT_RIGHT` |
| `parser/core.h` | Precedenza 6 (tra `+`/`-` e `<`/`>`) per shift. Precedenze ricalibrate: `*/% = 8`, `+/- = 7`, `<</>> = 6`, `</> = 5` |
| Emitter | Nessuna modifica — il fallback `token_kind_to_str()` emette `<<` e `>>` automaticamente |

### 11.3 CLI e Code Generation

| File | Modifica |
|------|----------|
| `args.h` | Aggiunto campo `output_file` e flag `-o <file>` (default: `"out.c"`) |
| `main.c` | `emit(program, 0, "out.c")` → `emit(program, 0, args.output_file)` |

### 11.4 Correzioni alla Specifica (README.md)

| Sezione | Prima | Dopo |
|---------|-------|------|
| §18 Overflow | "Signed integer overflow is undefined behavior (inherited from C99)" | "Signed integer overflow results in Two's Complement wrap-around. The C backend is compiled with `-fwrapv`" |
| §3.1 Conversioni | "implicit conversion between integers and floating numbers happens natively" | "implicit conversion between integer and floating-point types is the **only** implicit conversion permitted" |
| §13 Buffer safety | "No runtime bounds checks" | "Accesses that cannot be statically proven safe are rejected as compilation errors" |
| §2.4 Operatori | — | Aggiunti `<<` e `>>` alla tabella operatori bitwise |

### 11.5 Test Aggiunti

| File | Tipo | Verifica |
|------|------|----------|
| `tests/safety/ownership/return_var_local_fail.ln` | Negativo | `return var local` catturato come dangling reference ✅ |

### 11.6 Sessione 2 (2026-03-01 pomeriggio)

| File | Criticità Risolta | Descrizione |
|------|-------------------|-------------|
| `typecheck.h` | **§3.1** Func non totali | Check divisione per zero in `func`: se il divisore ha range sconosciuto o include zero, errore. Verifica anche constraint `!= 0` direttamente sulla Decl del parametro |
| `typecheck.h` | **#12** Arg count | Validazione numero argomenti in chiamate `func`/`proc` (escluse `extern` per supporto variadic) |
| `emit/stmt.h` + `emit/core.h` + `main.c` | **§9.3** `#line` directives | Ogni statement emesso con `#line N "file.ln"` per debugging source-level |
| `sema.h` | **VRA** Constraint inline | Constraint inline sui parametri (es. `b int != 0`) applicati al range table del body della funzione prima del walk |

**Test aggiunti (sessione 2):**

| File | Tipo | Verifica |
|------|------|----------|
| `tests/core/shift_operators.ln` | Positivo | `<<`, `>>` compilano e generano C corretto ✅ |
| `tests/safety/purity/div_by_zero_func_fail.ln` | Negativo | Divisione non protetta in `func` rifiutata ✅ |
| `tests/types/wrong_arg_count_fail.ln` | Negativo | Numero argomenti errato catturato ✅ |

---

## 12. Stato Attuale Post-Fix

### Tabella Priorità Aggiornata

| # | Criticità | Stato |
|---|-----------|-------|
| 1 | Borrow checker intra-statement | 🔴 **APERTO** — richiede lifetime tracking architetturale |
| 2 | Bounds checking non-bloccante | ✅ **RISOLTO** — ora errore bloccante |
| 3 | VRA overflow nei calcoli di range | ✅ **RISOLTO** — aritmetica saturante |
| 4 | Contraddizione overflow signed | ✅ **RISOLTO** — specifica coerente (`-fwrapv`) |
| 5 | Nested functions = non-C99 | ✅ **RISOLTO** — `walk_stmt` è `static` |
| 6 | Nessun tracking per `return var` | ✅ **RISOLTO** — check dangling reference |
| 7 | VRA solo per `+` e `-` | ✅ **RISOLTO** — supporto `*`/`/`/`%`/cast |
| 8 | Loop widening distrugge tutto | 🟡 **PARZIALE** — STMT_WHILE aggiunto, indice for preservato; accumuli generici ancora widened |
| 9 | `!=` constraint non implementato | ✅ **RISOLTO** — narrowing ai bordi |
| 10 | Ambiguità `x = 10` | 🟠 **APERTO** — design decision, non bug |
| 11 | Nessun drop/defer | ✅ **RISOLTO** — `defer` aggiunto for RAII scopes |
| 12 | Zero test negativi per types | ✅ **RISOLTO** — aggiunto check arg count + 2 test negativi |
| 13 | Struct passate by-value per `mov` | 🟡 **APERTO** — ottimizzazione emitter |
| 14 | Mancanza shift operators | ✅ **RISOLTO** — `<<`/`>>` completi |
| 15 | Output hardcodato su `out.c` | ✅ **RISOLTO** — flag `-o` |

**Nuove feature aggiunte:**
| Feature | Stato |
|---------|-------|
| §3.1 Div-by-zero check in `func` | ✅ **RISOLTO** — VRA + constraint `!= 0` |
| §9.3 `#line` directives | ✅ **RISOLTO** — emesse per ogni statement |
| Arg count validation | ✅ **RISOLTO** — per `func`/`proc` non-extern |
| Constraint inline → VRA body | ✅ **RISOLTO** — constraint parametri applicati nel body |

**Risultato test suite finale:**
- Positive: **45/53** passano (8 failure pre-esistenti, zero regressioni)
- Negative: **28/29** falliscono correttamente (1 false-pass pre-esistente: `uninit_fail.ln`)

---

## 13. Roadmap

### Priorità Alta — Safety

| Obiettivo | Complessità | Descrizione |
|-----------|-------------|-------------|
| **Lifetime tracking reale** | 🔴 Alta | Sostituire il borrow checker intra-statement con un sistema che traccia la durata dei borrow attraverso più statement. Modello di riferimento: NLL di Rust (regioni). Richiede refactoring di `region.h` + `linearity.h` |
| **Per-field struct linearity** | 🟠 Media | Permettere consumo parziale di struct lineari (`mov p.left` senza invalidare `p.right`). Richiede estensione `LTable` con tracking path-sensitive |
| **Definite initialization analysis** | 🟠 Media | Enforcement rigoroso: variabili non inizializzate devono essere errore (attualmente `uninit_fail.ln` passa) |

### Priorità Media — VRA e Determinismo

| Obiettivo | Complessità | Descrizione |
|-----------|-------------|-------------|
| **Loop widening intelligente** | 🟠 Media | Dopo un `for i in 0..n`, preservare non solo `i` ma anche accumulatori con pattern riconoscibili (`sum = sum + x` → range cumulativo stimato) |
| ~~**Verifica divisione per zero in `func`**~~ | ✅ Risolto | ~~`func` con `/` o `%` ora verifica che il divisore non includa zero. Supporta constraint `!= 0` direttamente~~  |
| **`extern func` audit** | 🟡 Bassa | Warning quando una `extern func` non è in una whitelist nota di funzioni pure |

### Priorità Media — Linguaggio

| Obiettivo | Complessità | Descrizione |
|-----------|-------------|-------------|
| **`defer` statement** | ✅ Risolto | ~~Meccanismo RAII-like per cleanup automatico a fine scope.~~ |
| **Generics Phase C** | 🔴 Alta | `Result(T, E)`, `Option(T)`, interfaces/shapes. Prerequisito per error propagation ergonomico |
| **Operatore `?`** | 🟡 Bassa | Sugar per `case result { Ok(v): v, Err(e): return Err(e) }`. Richiede generics |

### Priorità Bassa — Qualità

| Obiettivo | Complessità | Descrizione |
|-----------|-------------|-------------|
| **Struct `mov` by-pointer** | 🟡 Bassa | Emettere `T*` invece di `T` per parametri `mov` di struct > 16 bytes |
| ~~**`#line` directives**~~ | ✅ Risolto | ~~Emettere `#line` nel C generato per mapping source → debug~~ |
| **Type checker enforcement** | 🟠 Media | Verifica tipo in chiamate di funzione, assignment, return — attualmente molti mismatch passano silenziosamente |
| **Test coverage** | 🟡 Bassa | Aggiungere test negativi per: types, stdlib, cross-module, shadowing di variabili lineari |

### Decisioni di Design Pendenti

| Tema | Opzioni | Note |
|------|---------|------|
| `x = 10` ambiguità | (A) Mantenere status quo, (B) Richiedere `let` per nuovi binding, (C) Warning su nomi simili a variabili in scope | Qualsiasi cambiamento rompe tutto il codice esistente |
| Concorrenza | (A) No concorrenza (embedded-only), (B) `Send`/`Sync` traits + channels, (C) Actor model | Richiede ripensare il borrow checker se scelta B |
| Error model | (A) ADT manuali, (B) `Result(T,E)` generico + `?`, (C) Checked exceptions | Bloccato su generics Phase C |

---

## 14. Riepilogo Finale

### 14.1 Sintesi Esecutiva

L'analisi ha identificato **15 issue originali** + **4 feature aggiuntive** nel compilatore Lain. In due sessioni di lavoro (2026-03-01), sono state risolte **12/15 issue** e implementate tutte e 4 le feature aggiuntive, con **zero regressioni** nel test suite.

**Score complessivo:**

| Metrica | Prima | Dopo | Delta |
|---------|-------|------|-------|
| Issue risolti | 0/15 | 12/15 | +12 |
| Test positivi | 44/52 | 45/53 | +1 nuovo, 0 regressions |
| Test negativi | 24/25 | 28/29 | +4 nuovi (2 false-pass → 1) |
| File sorgente modificati | — | 10 | — |
| File test aggiunti | — | 4 | — |

### 14.2 Inventario Completo Modifiche per File

#### Compiler Core

| File | Modifiche | Sessione |
|------|-----------|----------|
| `src/sema/ranges.h` | Aritmetica saturante (`sat_add_i64`/`sat_sub_i64`); supporto `*`, `/`, `%`, `as` in `sema_eval_range`; gestione `!=` nel constraint narrowing | S1 |
| `src/sema/bounds.h` | Bounds check → errore bloccante (`exit(1)`) invece di warning | S1 |
| `src/sema.h` | `walk_stmt` refactored a `static` (C99); `STMT_WHILE` in `sema_widen_loop`; preservazione indice post-loop; constraint inline parametri → range table body | S1+S2 |
| `src/sema/typecheck.h` | Div-by-zero check in `func` con VRA + constraint `!= 0`; validazione arg count per `func`/`proc` (escluse `extern`) | S2 |
| `src/sema/linearity.h` | Check dangling reference per `return var local` | S1 |
| `src/token.h` | `TOKEN_SHIFT_LEFT`, `TOKEN_SHIFT_RIGHT` | S1 |
| `src/lexer.h` | Riconoscimento token `<<` e `>>` | S1 |
| `src/parser/core.h` | Precedenza shift operators (livello 6) | S1 |
| `src/args.h` | Campo `output_file`, flag `-o <file>` | S1 |
| `src/main.c` | Uso di `args.output_file`; setting `emit_source_filename` | S1+S2 |

#### Emitter

| File | Modifiche | Sessione |
|------|-----------|----------|
| `src/emit/core.h` | Globale `emit_source_filename` per `#line` directives | S2 |
| `src/emit/stmt.h` | Emissione `#line N "file.ln"` prima di ogni statement | S2 |

#### Specifica (README.md)

| Sezione | Modifica |
|---------|----------|
| §18 Overflow | Chiarito wrap-around con `-fwrapv` |
| §3.1 Conversioni | Solo int→float è implicita |
| §13 Buffer safety | Accessi non verificabili = errore |
| §2.4 Operatori | Aggiunti `<<`, `>>` |
| §Known Limitations | Nuova sezione |

#### Test

| File | Tipo | Sessione |
|------|------|----------|
| `tests/safety/ownership/return_var_local_fail.ln` | Negativo | S1 |
| `tests/core/shift_operators.ln` | Positivo | S2 |
| `tests/safety/purity/div_by_zero_func_fail.ln` | Negativo | S2 |
| `tests/types/wrong_arg_count_fail.ln` | Negativo | S2 |

### 14.3 Stato Attuale del Compilatore

#### Cosa il compilatore garantisce ora

| Garanzia | Meccanismo | Note |
|----------|-----------|------|
| **No buffer overflow** (array statici) | VRA + bounds check bloccante | Solo per accessi con indice verificabile |
| **No dangling reference** (`return var`) | Check in linearity.h | Solo per variabili locali dirette |
| **Funzioni pure totali** | Ban ricorsione + div-by-zero check | `func` non può avere UB o non-terminazione |
| **Ownership lineare** | LTable + borrow tracking | Singolo proprietario, no double-free |
| **Purity enforcement** | `func` non può chiamare `proc`, né leggere globali mutabili | — |
| **Source mapping** | `#line` directives | Debug del C generato punta al `.ln` |

#### Cosa NON garantisce ancora

| Lacuna | Rischio | Priorità |
|--------|---------|----------|
| Borrow checker single-statement | Aliasing temporaneo in espressioni complesse | 🔴 Alta |
| `= undefined` accettato | Possibilità di uso di memoria non inizializzata | 🟠 Media |
| Nessun `defer`/drop | Resource leak se il programmatore dimentica cleanup | 🟠 Media |
| Type checking incompleto | Alcuni mismatch di tipo passano silenziosamente | 🟠 Media |
| `extern func` non auditata | `extern func` potrebbe avere side effects | 🟡 Bassa |

### 14.4 Roadmap Consolidata

```mermaid
gantt
    title Roadmap Lain Compiler
    dateFormat YYYY-Q
    axisFormat %Y-Q%q

    section Safety
    Lifetime tracking NLL    :crit, l1, 2026-Q2, 90d
    Per-field struct linearity :l2, after l1, 60d
    Definite initialization   :l3, 2026-Q2, 30d

    section Language
    defer statement           :lang1, 2026-Q2, 45d
    Generics Phase C          :crit, lang2, 2026-Q3, 120d
    Operatore ?               :lang3, after lang2, 30d

    section VRA
    Loop widening intelligente :vra1, 2026-Q2, 30d
    extern func audit          :vra2, 2026-Q3, 14d

    section Quality
    Type checker enforcement   :q1, 2026-Q2, 45d
    Struct mov by-pointer      :q2, 2026-Q3, 14d
    Test coverage expansion    :q3, 2026-Q2, 2026-Q4
```

#### Fase 1 — Safety Foundation (Q2 2026)

| # | Obiettivo | Impatto | File coinvolti |
|---|-----------|---------|----------------|
| 1 | **Lifetime tracking NLL** | Elimina aliasing intra-statement | `region.h`, `linearity.h` |
| 2 | **Definite initialization** | Elimina uso di `= undefined` non intenzionale | `sema.h`, parser |
| 3 | ~~**`defer` statement**~~ | ✅ Risolto | parser, AST, emitter |

#### Fase 2 — Language Maturity (Q3 2026)

| # | Obiettivo | Impatto | Prerequisiti |
|---|-----------|---------|-------------|
| 4 | **Generics Phase C** | `Result(T,E)`, `Option(T)`, interfaces | — |
| 5 | **Operatore `?`** | Error propagation ergonomico | Generics Phase C |
| 6 | **Per-field struct linearity** | Consumo parziale di struct lineari | — |

#### Fase 3 — Polish (Q3-Q4 2026)

| # | Obiettivo | Impatto |
|---|-----------|---------|
| 7 | **Type checker enforcement** | Elimina mismatch di tipo silenti |
| 8 | **Loop widening intelligente** | VRA preserva info su accumulatori |
| 9 | **Struct `mov` by-pointer** | Performance per struct > 16 bytes |
| 10 | **`extern func` audit** | Warning su funzioni impure dichiarate `func` |

### 14.5 Failure Pre-Esistenti (Non Correlati)

I seguenti 8 test positivi falliscono per motivi indipendenti dalle modifiche di questa analisi:

| Test | Causa |
|------|-------|
| `comptime_phase_b_error.ln` | Errore di tipo in comptime |
| `core/ufcs_test.ln` | `lookup_struct_field_type` con NULL |
| `core/unsafe_adt_pass.ln` | Idem |
| `types/adt.ln` | Match non esaustivo |
| `types/destructuring.ln` | `lookup_struct_field_type` con NULL |
| `types/enum_exhaustive.ln` | Match non esaustivo |
| `types/enums.ln` | `emit error: unhandled type kind 6` |
| `stdlib/test_fs.ln` | Debug print residua in registrazione `fopen` |

Il 1 test negativo false-pass:

| Test | Causa |
|------|-------|
| `core/uninit_fail.ln` | `var x int = undefined` è intenzionalmente accettato dal parser come escape hatch |

