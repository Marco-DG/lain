# Analysis 5: Borrow Checker & Overlapping Borrows in Lain

## 1. Origine del Problema
Durante il refactoring del lexer abbiamo provato ad usare questa astrazione per semplificare il codice:
```lain
while is_alnum(peek(l)) { 
    consume(var l) 
}
```
Questa struttura ha generato l'errore:
> `bounds warning: index range unknown, cannot verify against length 1`
> `borrow error: cannot borrow 'l' as mutable because it is already borrowed`

## 2. È una mancanza del linguaggio o una feature?
Come descritto in `specification.md` (sezione 5.3 Borrowing Rules - Read-Write Lock), Lain stabilisce queste regole ferree a compile-time:
1. I prestiti in sola lettura (`shared borrows`) possono essere multipli e contemporanei.
2. Esiste **esattamente un solo** prestito mutabile alla volta.
3. **Shared e mutable borrows non possono coesistere** per la stessa variabile nello stesso scope.

Quindi, dal punto di vista concettuale del "Read-Write Lock" per la *memory safety*, **il compilatore ha agito correttamente** applicando le regole:
- Eseguendo il `while`, il compilatore valuta la condizione `peek(l)`, accendendo uno *shared borrow* (prestito in sola lettura) sulla variabile `l`.
- Quando entra nel blocco del `while` e incontra `consume(var l)`, richiede un `mutable borrow` sulla stessa risorsa `l`.
- Siccome `l` è già in prestito condiviso per la valutazione della condizione, Lain interviene e ferma la compilazione per prevenire data-race e pointer invalidation.

## 3. La vera "mancanza": Assenza di Non-Lexical Lifetimes (NLL)
Se l'implementazione del Read-Write Lock è impeccabile, **la durata del prestito (Lifetime) è il vero problema**.

Questa è un'effettiva *mancanza architetturale dell'attuale implementazione del compilatore di Lain*. 
Al momento, Lain implementa un Borrow Checker di tipo **"Lexical"**. Significa che la validità di un prestito è legata all'intero blocco (o statement) in cui nasce. Il prestito in sola lettura generato dalla funzione `peek(l)` all'interno della condizione del ciclo non termina non appena l'espressione è stata valutata, ma **rimane attivo per l'intera esecuzione del blocco del while**.

Prima dell'edizione 2018 (che introdusse il NLL - Non-Lexical Lifetimes), anche **Rust presentava lo stesso esatto problema**, causando immensa frustrazione negli sviluppatori che si trovavano le variabili bloccate senza motivo.
In un linguaggio moderno con NLL, il compilatore capirebbe tramite una "control-flow graph analysis" che il prestito di `peek(l)` è usato unicamente al momento della valutazione condizionale e il suo scopo scade *esattamente* prima di entrare nel corpo iterativo, permettendo così il prestito mutabile richiesto da `consume(var l)`.

## 4. Soluzioni e Direzione Futura
Questo comportamento **non va cambiato in `specification.md`** in quanto le logiche di Ownership sono corrette. Ciononostante, affinché Lain possa supportare codice elegante e privo di frizioni, bisogna aggiornare l'engine del Borrow Checker.

**Prossimi Step nello sviluppo del compilatore:**
- **Lifetime a granularità di espressione (Expression-Level Lifetimes):** Modificare `sema/borrowck.h` affinché prestiti temporanei usati come argomenti in `EXPR_CALL` (come `peek(l)` ) sgancino il proprio lifetime immediatamente dopo l'esecuzione della chiamata, ovvero al termine formale dell'espressione, piuttosto che in coda allo statement padre (`STMT_WHILE` o `STMT_IF`).
- Fino ad allora, bisognerà appoggiarsi a letture temporanee immagazzinate in variabili esplicite fuori dai loop, oppure adottare costrutti *inline* e array/slice bound checking automatico (es. `l.pos += 1`) che mascherano implicitamente la problematica bypassando chiamate a funzioni ausiliarie.
