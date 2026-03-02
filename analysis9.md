# Report Allineamento Specifica-Implementazione: Lain
**Data Esecuzione Investigativa**: [Inserire Data]

> **Nota**: Questo documento raccoglierà i risultati dell'analisi parallela dettagliata definita in `roadmap1.md`. Gli agenti riporteranno qui ogni discrepanza individuata tra il file `README.md` (Spec) e la directory `src/` (Codice Sorgente).

## Agente 1: Lexical, Sintassi e Tipi Base
- [Spec §3.1 e §8.9] "Implicit conversion between integer and floating-point types is the only implicit conversion permitted... Every integer conversion must be explicitly annotated" vs [src/sema/typecheck.h]
  - **Dettaglio**: In `typecheck.h` non c'è traccia di un enforcement rigoroso in `EXPR_ASSIGN` o `EXPR_CALL` (passing args) che confronti `LHS.type == RHS.type`. La semantica attuale del compilatore *si fida* ciecamente dell'auto-cast di C. Non c'è alcun costrutto nel compiler (attualmente) che blocchi un utente dal passare un `i8` ad un argomento `int` senza usare `as`.
  - **Impatto**: **Alto** (La promessa di type-safety rigorosa senza implicit casting è attualmente elusa).
- [Spec §3.8 e §3.9] "Variables cannot be declared of type void... Instantiating [opaque types] by value is a compile error" vs [src/parser/type.h & stmt.h]
  - **Dettaglio**: Non ho trovato controlli espliciti in fase parser o sema che rigettino ast_node typed `void` o l'instanziazione di struct esterne per valore. Se l'utente scrive `var x void`, il compilatore cercherà un tipo "void" come identificatore base che, se inesistente, crasherà, ma non con l'errore semantico promesso.
  - **Impatto**: **Medio**.
- [Spec §8.8] "Using == or != directly on struct... is a compile error." vs [src/sema/typecheck.h EXPR_BINARY]
  - **Dettaglio**: Il `EXPR_BINARY` non controlla la natura del tipo sinistro e destro prima di accettare `==` o `!=`. Quindi l'errore promesso dalla specifica ("compile error") viene delegato al backend C, risultando in un errore confuso "invalid operands to binary == (have 'Point' and 'Point')".
  - **Impatto**: **Basso** (l'errore finale blocca comunque la compilazione, ma compromette l'ergonomia dei messaggi utente e la promessa di early-checking del compilatore Lain).

## Agente 2: Variabili, Mutabilità e Ownership
- [Spec §4.6] "Shadowing is permitted... the previous variable is restored" vs [src/sema/scope.h]
  - **Dettaglio**: L'implementazione in `scope.h` utilizza un sistema robusto basato su arene salvando il puntatore di testa (bucket head) all'inizio del blocco (`sema_push_scope`) e ripristinandolo in uscita (`sema_pop_scope`). Questo approccio garantisce perfettamente lo shadowing delle variabili locali senza overhead o conflitti di ID allocato nella symbol table. Nessuna discrepanza rilevata.
  - **Impatto**: **Nullo**.
- [Spec §4.5] Ambiguity in `x = 10` parsing vs [src/parser/stmt.h]
  - **Dettaglio**: La logica di parsing in `stmt.h` conferma quanto scritto in `README.md`. L'assenza di `var` instrada un'identificatore in `parse_decl_stmt()` che setta la mutabilità a `false`. Per le assegnazioni pure c'è l'operator lookup `=`. Questo aderisce alla sintassi descritta con precisione prototipica. Nessuna discrepanza.
  - **Impatto**: **Nullo**.
- [Spec §5.7] "It's illegal to return a mutable reference to a locally scope variable" vs [src/sema/linearity.h]
  - **Dettaglio**: Il check per i *dangling pointer* associato al `return var` esiste in `STMT_RETURN`. Controlla la forma `EXPR_MUT(EXPR_IDENTIFIER)` per verificare se è un local non parameter. **Tuttavia**, il controllo è superficiale (controlla un livello di AST expr). Se l'utente scrive: `return var local_struct.array[0]`, l'AST nodo radice non è `EXPR_IDENTIFIER`, ma `EXPR_INDEX / EXPR_MEMBER`, passando erroneamente il check. 
  - **Impatto**: **Alto** (Dangling pointer in fuga alla fase semantica).

## Agente 3: Funzioni, Control Flow e Pattern Matching
- [Spec §6.1] "Pure functions strictly forbid... reading or writing mutable global variables [and] calling impure procedures" vs [src/sema/resolve.h & typecheck.h]
  - **Dettaglio**: In `typecheck.h`, `EXPR_IDENTIFIER` controlla esplicitamente il flag `is_global` e `is_mutable` rigettando l'accesso con un fatal error nei blocchi `func`. In `resolve.h`, l'assegnazione controlla lo stesso. Inoltre `typecheck.h` vieta le chiamate a `proc` e i loop `while` nelle `func`. Tutto implementato come promesso.
  - **Impatto**: **Nullo**.
- [Spec §7.6] "Match statements must test every possible value... or provide an else branch" vs [src/sema/exhaustiveness.h]
  - **Dettaglio**: La logica in `match_check_enum_exhaustiveness` e `match_check_bool_exhaustiveness` processa scrupolosamente varianti enum e literal booleani. I tipi interi, non avendo branch specifici, fallthrough a `false` ritornando costantemente "non-exhaustive" a meno che non sia presente un branch `else` (illegale omettere l'else su interi).
  Tuttavia, il backend non sembra supportare exhaustiveness per Pattern ADT (es. matchare tutti i possibili shape).
  - **Impatto**: **Basso** (corretto per i tipi previsti).

## Agente 4: Operator Precedence, Equazioni ed Expressions
- [Spec §8.7] "1. `* / %`, 2. `+ -`, 3. `<< >>`, 4. `<`, `>`, `<=`, `>=`, 5. `== !=`, 6. `&`, 7. `^`, 8. `|`, 9. `and`, 10. `or`" vs [src/parser/core.h get_precedence]
  - **Dettaglio**: La tabella di parsing implementata differisce leggermente dal README per gli operatori bitwise: il parser assegna a `|` e `^` la *stessa* precedenza (2), mentre il README li separa (7 e 8).
  - **Impatto**: **Medio** (Pesa sulle parentesi implicite attorno a complesse operazioni bitwise).
- [Spec §18.2] "Lain defines integer overflow as wrapping... using `-fwrapv` on C backends" vs [src/main.c]
  - **Dettaglio**: Lain emette puro codice C su file, ma non invoca nativamente il linker/compiler sottostante. Lo script di build (`run_tests.sh`) o l'utente finale deve fisicamente ricordarsi di passare `-fwrapv` a Cosmopolitan/GCC. Questa non è un'opzione enforced hardcoded nel binario Lain stesso.
  - **Impatto**: **Basso** (Si risolve aggiornando i tooling ausiliari o includendo pragmas GCC directly nel file emit.c se supportati).

## Agente 5: VRA, Tipo di Constraint e Compiler Directives
- [Spec §9] "All arrays and slices natively carry length metadata... checked via Value Range Analysis" vs [src/sema/bounds.h]
  - **Dettaglio**: In `bounds.h`, il check sull'accesso in index alle slice e agli array deduce min/max tramite il `RangeTable` originato da `ranges.h`. Se la lunghezza dell'array è statica o legata a string literal (`sentinel_len`), blocca le out-of-bounds al compile-time. Purtroppo per le dynamic length avvisa giustamente l'utente di optare per `for i in 0..arr.len`, onorando il zero-overhead principle (niente guardie hardware occulte in runtime).
  - **Impatto**: **Basso** (L'implementazione è robusta sulle size const, ma deficitaria sulle dynamic a causa di assenza di check cross-file).
- [Spec §15.2] "Use `#[no_mangle]` before a declaration to export it" vs [src/parser/decl.h & emit/decl.h]
  - **Dettaglio**: Nel codice del parsing `decl.h` non esiste alcun check per l'attributo testuale `#[no_mangle]`. Tutte le function declarate (`parse_func_decl`) usano `c_name_for_id` in emissione (`emit/decl.h:208`), forzando cross-module names via `module_path_func`.
  - **Impatto**: **Alto** (La FFI esportata e la linkabilità delle libreirie costruite dal programmatore sono attualmente inaccessibili).

## Sommario Esecutivo
Tutte le ispezioni su strada sono concluse. L'obiettivo macro "Check 100% dell'implementazione" in `roadmap1.md` ha rivelato scollature su: Implicit Casting C (Alto exp), Exhaustiveness ADT Pattern (Basso), `return var` dangling escapes (Alto), `#[no_mangle]` fallimento (Alto).

---

## Analisi e Roadmap delle Correzioni (Fase 6)

L'indagine ha identificato cinque macro-aree critiche dove il compilatore Lain diverge dalla specifica tecnica stabilita nel `README.md`. Di seguito un'analisi strutturata e un piano di risoluzione prioritizzato.

### Analisi degli Issue

1.  **Dangling Pointers in `return var` (Sicurezza di Memoria - Alta Priorità)**:
    Il compilatore attualmente impedisce il ritorno di reference mutabili a variabili locali usando un check superficiale sull'AST (`EXPR_MUT` con `EXPR_IDENTIFIER` diretto). Tuttavia, omettendo la ricorsione su espressioni annidate (come i membri di struct o gli indici di slice: `EXPR_MEMBER`, `EXPR_INDEX`), espone il compilatore a dangling pointers fatali e non segnalati. Questo rompe la garanzia core zero-cost safety di Lain.

2.  **Cast Impliciti e Uguaglianza Strutturale (Type Safety - Alta Priorità)**:
    La specifica vieta esplicitamente i cast impliciti per favorire una gestione rigorosa del typing esplicito tramite `as`. L'attuale fase semantica nel typechecker non impone questa restrizione, delegando ciecamente il tutto al backend C (che accetta le promozioni e le conversioni silenziose). Ugualmente, delegando la comparazione strutturale (`==` su struct) al C, si rompe l'ergonomia user-facing, restituendo errori arcani al momento dell'invocazione GCC invece che un chiaro errore Lain pre-analisi.

3.  **Direttiva `#[no_mangle]` Mancante (FFI & Linkability - Alta Priorità)**:
    Senza il supporto per le dichiarazioni `#[no_mangle]`, è teoricamente impossibile creare interfacce esportabili esterne (librerie dinamiche `.so`/`.dll`) adatte a C-interop fluido con codice nativo. Attualmente tutto lo shading delle funzioni passa forzatamente da `c_name_for_id`, distruggendo la compatibilità link external.

4.  **Exhaustiveness dei Match Interi (Control Flow Safety - Media Priorità)**:
    Lo switch statement `match` per tipi interi manca dell'obbligo formale - imposto dalla specifica - di presentare una clausola fallback `else:` quando non può coprire l'intera estensione del dominio. Attualmente, la verifica exhaustiveness degli interi fa silenziosamente un bypassing o return erroneo. 

5.  **Precedenza Operatori Bitwise (Correttezza Espressioni - Bassa Priorità)**:
    L'attuale funzione `get_precedence` del Pratt Parser fonde la gerarchia sintattica tra gli operatori Pipe (`|`) e Caret (`^`). Si scontra col behavior documentato e il comune C-behavior che vuole lo XOR applicato ad un livello sfalsato rispetto all'OR.

### Roadmap di Risoluzione (Task Execution)

Per riportare rapidamente il compilatore al 100% di compliance col README, l'esecuzione seguirà questo ordine:

- [x] **1. Sicurezza e Type System (Sema Core)**
   - **1.1** ✅ Implementato il traverse profondo in `STMT_RETURN` in `resolve.h`/`linearity.h` per bloccare reference mutabili a nodi `EXPR_MEMBER` o `EXPR_INDEX` ancorati a identificatori locali.
   - **1.2** ⏳ Rafforzare `typecheck.h`: bloccare le assegnazioni `target = expr` se `target->type != expr->type` per i tipi numerici (forzando l'uso di `EXPR_CAST`). (Deferred: richiede una matrice di compatibilità tipi completa)
   - **1.3** ✅ Bloccato esplicitamente i token `==` e `!=` se applicati a Struct/Enum (sollevando fatal error con messaggio chiaro).
- [ ] **2. Compiler Directives & FFI**
   - **2.1** ⏳ Introdurre logic parsing per `#` `[` `no_mangle` `]` come decoratore per `func`/`proc`/`extern`. (Deferred: richiede nuovi token nel lexer)
   - **2.2** ⏳ Modificare `c_name_for_id` nell'emitter per preservare il raw name originale se il flag `no_mangle` è attestato sulla dichiarazione AST.
- [x] **3. Quality of Life & Syntax Fix**
   - **3.1** ✅ L'algoritmo di exhaustiveness in `sema_check_match_exhaustive` già obbliga la presenza esplicita di `else:` nel parsing match per interi (verificato).
   - **3.2** ✅ Ritoccato `get_precedence()` per gli operatori bitwise conformando `|` (3) e `^` (4) e `&` (5) ai layer della specifica 8.7.
   - **3.3** ✅ Confermato che `-fwrapv` è già passato in `run_tests.sh` (riga 24) al backend C.
