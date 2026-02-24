# Lain Language — Deep Architecture Analysis & Roadmap (v3)

**Data**: February 2026
**Contesto**: Questa analisi si basa sullo stato attuale del compilatore Lain (dopo le correzioni iniziali esplose in `analysis2.md`) e sui test implementati, con l'obiettivo di "fissare nella pietra" le semantiche del linguaggio e preparare il terreno per un refactoring architetturale (Phase 2).

---

## 1. Stato dell'Arte (Post-Fase 1)

Il compilatore ha fatto notevoli passi avanti nell'enforce delle regole di ownership, purity e bounds checking statico. Le keyword e i tipi fondamentali (`case`, `bool`, `i32`, `as`) sono operativi e correttamente parserizzati.

Tuttavia, sotto la superficie, il compilatore soffre di **problemi strutturali** che limitano la validità delle sue stesse garanzie. Il linguaggio è "safe" in teoria, ma l'implementazione attuale lascia aperte pericolose falle.

### Il Paradosso della Linearità
Lain garantisce la safety di memoria senza GC usando i tipi lineari (`mov`). Questo funziona perfettamente in una singola funzione isolata. Ma il **sistema di import** e la risoluzione dei nomi non conservano le firme corrette (nello specifico le annotazioni `mov` sui parametri formali) quando una funzione viene importata da un altro modulo. Come risultato, chiamare una funzione esterna (che richiede `mov`) spesso fallbacka a una eurisitica fragile ("consuma il parametro solo se il chiamante usa esplicitamente `mov x` altrimenti assumi prestito"). Questo invalida le garanzie globali.

---

## 2. Analisi dei Problemi Architetturali (Pre-Fase 2)

Questi difetti richiedono modifiche profonde nel modo in cui l'AST e l'Analisi Semantica sono costruiti. Non sono semplici bug, ma limiti di design dell'attuale compilatore in C.

### 2.1 Scope System "Piatto" (Mancanza di Block Scoping)
L'analizzatore semantico (`scope.h`) divide il mondo binarmente: o Globale, o Locale. Non esiste un concetto di "stack di scope".
- **Sintomo**: Variabili dichiarate dentro un `if` o `for` sono visibili fuori dal blocco (fino alla fine della funzione). Non è possibile shadow-are le variabili in blocchi innestati.
- **Correzione strutturale**: L'analizzatore semantico deve implementare uno "scope stack". Ogni apertura di `{` pusha un nuovo environment table. Alla chiusura `}`, tutte le variabili dichiarate in quello scope locale devono essere processate (e controllate per linearità! le variabili `mov` non passate ad altri devono sollevare errore di "dropped without consume"), poi la tabella deve essere distrutta.

### 2.2 Perdita delle Informazioni di Ownership nei Moduli Importati
- **Sintomo**: Un file importa `std.fs`, chiama `close_file(mov f)`. Il compiler, che espande gli import non mantenendo i metadati di funzione perfetti, non riconosce che `close_file` ha un constraint lineare di base.
- **Correzione strutturale**: Il sistema di resolving (`resolve.h`) deve costruire una Root Symbol Table persistente attraverso i moduli, memorizzando le firme complete (`DECL_FUNCTION` e `DECL_PROCEDURE`) con i flag `PARAM_MOV` per tutti i moduli compilati/importati. L'euristica "indovina dal sito di chiamata" in `linearity.h` deve essere rimossa e sostituita da una rigida Type Signature Matching.

### 2.3 `func main()` vs `proc main()`: Coerenza del Modello di Purity
- **Sintomo**: Decine di file di test dichiarano `func main() int` e poi chiamano `libc_printf(...)`, che effettua I/O.
- **Dilemma**: L'I/O è un side effect. Una funzione pure (`func`) non può avere side effects. Quindi `main` DEVE essere dichiarata `proc main() int`. Il compilatore attualmente perdona `main` ignorando questa limitazione.
- **Correzione strutturale**: Fissare "nella pietra" che `main` fa I/O con il sistema operativo, quindi è sempre `proc`. Aggiornare l'analizzatore semantico per rifiutare `func main` e aggiornare tutti i ~40 vecchi file di test per usare `proc main()`.

### 2.4 Bounds Checking limitato alle costanti e widening dei Loop
- **Sintomo**: L'engine di Value Range Analysis (VRA) traccia i limiti in modo polinomiale, ma azzera i constraint ogni volta che una variabile entra in un loop ("loop widening").
- **Soluzione futura**: VRA necessita di un costrutto "Loop Invariant Annotation", o in alternativa un modo per dimostrare che, se un iteratore parte da 0 e fa `< 10`, alla fine del loop vale ESATTAMENTE 10. Per la Fase 2, dobbiamo accettare questo limite ma documentarlo come tale.

### 2.5 L'Hack di `libc_printf` = `printf`
- **Sintomo**: Nel generatore C (emitter), Lain si appoggia su un hack del preprocessore C (`-Dlibc_printf=printf`) nel bash script di test.
- **Correzione strutturale**: Fissare una logica solida per la FFI (Foreign Function Interface). Il parametro `extern func printf` in Lain dovrebbe poter specificare il mangled name del C. Ad esempio: `extern proc printf(fmt *u8, ...) int @linkname("printf")`.

---

## 3. RoadMap delle Correzioni C (Piano Operativo)

Per portare il linguaggio Lain allo stato di "Produzione" (Fase 2 + Fase 3), si raccomanda la seguente roadmap implementativa per il compilatore in C:

| Priorità | Componente | Azione | Note |
|:---|:---|:---|:---|
| 🔥 **Alta** | `sema.h` & `scope.h` | **Block Scoping**: Risolvere `clear_locals` inserendo array di `ScopeTable*` | Necessario per un linguaggio affidabile. Le variabili non devono "fuoriuscire" dagli `if`. |
| 🔥 **Alta** | `sema.h` & `import` | Risoluzione del **Cross-Module Ownership**: esportare le firme (signature) nei moduli per evitare errori heuristici sulle funzioni aliene. | Senza questo, i tipi lineari mentono e i leak sfuggono alle maglie. |
| 🟠 **Media** | *Tutti i Test* | Refactoring Massivo: Sostituire `func main()` con `proc main()`. Sostituire `extern func printf` con `extern proc printf`. | Questo forzerà il compilatore a testare davvero l'enforcement della boundary tra `func` e `proc`. |
| 🟠 **Media** | Lexer/Parser | **Migliorare messaggi di errore**: Implementare i campi `line` e `col` su `LEntry` dentro `linearity.h` | Fondamentale per la Developer Experience (DX). L'utente deve sapere su quale riga ha droppato un handle non consumato. |
| 🔵 **Bassa** | `AST`/`Emitter` | **Operatore Address-of (`&`)**: Implementare operator unario `&` bloccato da un sub-sema check (legal solo entro blocchi `unsafe`). | Permette all'utente di interfacciarsi robustamente con C e fare mutazioni complesse localmente. |
| 🔵 **Bassa** | `std/fs` | **Fix `close_file()`**: Riparare il bug dove, se l'handle di rete = 0, il pointer non è propriamente invalidato/freeato. | Una correzione della standard library. |

## 4. Nuove Specifiche Semantiche Definite

Lain è adesso "congelato". Le semantiche esatte definite sono:
1. Non vi è _nessuna_ uguaglianza strutturale automatica per Tuple, Struct, ADT;
2. Ordine stringente left-to-right degli argomenti;
3. Strict assignment parsing (per differenziare una nuova variabile let e una var esistente in base alla scope reference).
4. Un parametro mutable referenziato (`var x`) non può essere "leakato" localmente ritornandolo indietro tranne che come riferimento ad esso (`return var x`) con liftime legate, opzione al momento formalmente proibita/riservata su variabili locali. Si raccomanda di tornare i reference *solo* di argomenti function passati come `var`.

> Le specifiche del linguaggio (`specification.md`) riflettono ora questo stato definitivo, incluse le ambiguità finora non documentate. Non bisogna introdurre costrutti ad alto livello non analizzati senza prima sistemare la base della semantica in C (Block scoping e cross-module linearity).
