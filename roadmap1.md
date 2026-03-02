# Lain Spec vs Implementation Roadmap (roadmap1.md)

## Obiettivo Principale
L'obiettivo di questa roadmap è guidare un pool di agenti AI che lavoreranno in parallelo per verificare **al massimo grado di profondità possibile** la coerenza al 100% tra la specifica del linguaggio Lain (`README.md`) e la sua attuale implementazione (`src/*`).

I risultati di questa indagine dovranno essere registrati nel file `findings1.md` secondo un formato strutturato.

---

## Istruzioni Generali per gli Agenti

1. **Focus sulla Discrepanza**: Il vostro compito non è giudicare il design del linguaggio, ma trovare *esattamente* dove il compilatore fa qualcosa di diverso da quanto scritto nel `README.md`, o dove il `README.md` promette regole che il compilatore non enforcing.
2. **Ispezione ad Alto Rigore**: Dovete spulciare il codice C del compilatore (AST, parser, typechecker, emitter) e confrontarlo riga per riga con le affermazioni della specifica.
3. **Formato Output**: Ogni discrepanza trovata deve essere registrata in `findings1.md` specificando:
   - Riferimento alla Sezione del README.
   - Riferimento al file/riga del compilatore (es. `src/sema/typecheck.h:120`).
   - Descrizione del disallineamento (Spec dice X, Implementazione fa Y).
   - Gravità (Bassa/Media/Alta).

---

## Aree di Indagine Assegnate (Parallel Tasking)

Gli agenti dovranno dividersi il lavoro secondo le seguenti macro-aree di indagine:

### Agente 1: Lexical, Sintassi e Tipi Base (Sezioni 2 & 3)
- **Keyword e Operatori**: Verificare in `src/lexer.h` e `src/token.h` che tutte le keyword elencate in §2.1 siano effettivamente parsate e che non ci siano keyword parserate ma non documentate.
- **Tipizzazione Numerica**: Controllare in `src/sema/typecheck.h` le regole di casting (`as`). La specifica §3.1 e §8.9 afferma che "implicit conversion between integer and floating-point types is the only implicit conversion permitted". È vero? Cosa succede se passo un `i8` a una funzione che vuole `int`?
- **Stringhe e Sentinel**: In `src/ast.h` e `src/emit/`, verificare la struttura di `u8[:0]`. La specifica (§2.3) promette l'accesso ai campi `.len` e `.data`. Il compilatore risolve correttamente questi membri per le stringhe letterali?
- **Opaque Types e Void**: In §3.8 e §3.9, controllare se davvero è impossibile dichiarare `var x void` o instanziare un `extern type` by value nel parser/typechecker.

### Agente 2: Variabili, Mutabilità e Ownership (Sezioni 4 & 5)
- **Shadowing**: La specifica §4.6 dice che lo shadowing è permesso. Il compilatore (`src/sema/scope.h`) gestisce correttamente il pop dello scope ripristinando la variabile esterna senza conflitti di ID di memoria?
- **Ambiguity `x = 10`**: In §4.5, si descrive la disambiguazione tra nuovo binding e assignment. Verificare l'esatta logica in `src/parser/stmt.h` o `src/sema/resolve.h` per confermarne il comportamento descritto.
- **Borrow Checker Limitazioni**: Oltre a quanto già noto sull'assenza di NLL reale, cercare loop-hole specifici nel borrow checker (`src/sema/linearity.h`) riguardanti le struct non lineari vs lineari (annotazioni `mov` sui field §5.4).
- **Return Var**: In §5.7 si menziona `return var`. Ora che il check per la restrizione locale è stato implementato, ci sono altri modi in cui un `return var` può generare un dangling pointer non intercettato (es. returning una reference mutabile acquisita tramite call)?

### Agente 3: Funzioni, Control Flow e Pattern Matching (Sezioni 6 & 7)
- **Purity Enforcement**: In §6.1 e `src/sema/` verificare se una `func` pura ha davvero il divieto *assoluto* di accedere a variabili globali mutabili (non locali).
- **Match Exhaustiveness**: La specifica §7.6 dettaglia tre regole stringenti di exhaustiveness per il costrutto `case`. In `src/sema/resolve.h` (o simili) il check è davvero implementato come descritto per Enum, ADT e Integers? Cosa succede se non si mette l'arm `else` su un match intero? C'è il throw dell'errore formalizzato?
- **If/Else/Match scoping**: Verificare se le variabili dichiarate all'interno di un arm `case` o di un blocco `if` influenzano l'LTable (Linearity Table) globale in modo scorretto o se il branching scoping funziona a tenuta stagna.

### Agente 4: Operator Precedence, Equazioni ed Expressions (Sezioni 8 & 18)
- **Operatori ed Espressioni**: In `src/parser/core.h` confrontare la tabella delle precedenze di §8.7 con quella effettivamente implementata dalla logica ricorsiva del Pratt Parser.
- **Structural Equality**: In §8.8 viene sancito l'errore a compile-time per `==` su struct/array. `src/sema/typecheck.h` enforces questa regola correttamente?
- **Overflow and Division**: §18 dice che `-fwrapv` viene iniettato automaticamente. Dove e come (o *se*) `src/main.c` effettua l'injection del flag verso il backend C/cosmocc? (Questo potrebbe essere falso e da correggere o implementare nell'invocazione child-process).

### Agente 5: VRA, Tipo di Constraint e Compiler Directives (Sezioni 9, 10 & 11)
- **VRA Boundary Check**: Verificare l'architettura spiegata in §9.6. I check del tipo `a int > b` funzionano davvero nel compilatore odierno? O il parser accetta solo constraint numerici letterali (`int > 0`)?
- **Array e VRA `in`**: Il constraint `in arr` descritto in §9.3 è effettivamente transpilato nel syntactic sugar `i >= 0 and i < arr.len` come promesso?
- **Modulo Resolving**: §10.1 documenta l'import di moduli e l'uso dell'aliasing `as`. `src/main.c` gestisce davvero i namespace prefissati o butta tutto a global level?
- **C-Interop/Variadics**: Verificare in §11.4 e nel typechecker l'accettazione corretta della sintassi `...` solo in ambito `extern func`.

---

## Formato del file `findings1.md`
Tutti gli agenti, una volta conclusa l'ispezione della propria area, apriranno e compileranno il file `findings1.md` seguendo il template qui di seguito:

```markdown
# Report Allineamento Specifica-Implementazione: Lain
**Data Esecuzione Investigativa**: [Inserire Data]

## Agente 1: Lexical e Tipi Base
- [Spec §X.X] "..." vs [src/path:line] "..."
  - **Dettaglio**: ...
  - **Impatto**: [Alto/Medio/Basso]

## Agente 2: Variabili e Ownership
...

## Agente 3: Funzioni e Control Flow
...

## Agente 4: Espressioni
...

## Agente 5: Analisi Statica e Backend
...
```

**Obiettivo finale per l'orchestratore**: Ottenere una mappatura a 360° senza zone d'ombra. Se non verranno trovate discrepanze da un agente nel suo layer, l'agente dovrà scrivere "Nessuna Discrepanza Rilevata: La specifica è congruente con il codice.".
