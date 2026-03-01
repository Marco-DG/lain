# Genesi e Sviluppo di Lain: Iteration 1 - Comptime Generics

Questo walkthrough delinea le modifiche apportate alla base di codice Lain per completare la **Fase A: Generici al Call-Site (Monomorfizzazione Automatica)** del costrutto [comptime](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h#511-519).

## Obiettivo della Fase A
L'obiettivo di questa fase era consentire alle funzioni di accettare parametri meta-tipo con `comptime T type` e usare `T` all'interno della firma della funzione e nel corpo. Quando il chiamante invia un tipo (es. [int](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast_print.h#68-74)), il compilatore crea automaticamente e trasparentemente un'istanza *deep-copy* (monomorfizzata) della funzione originale, sostituendo `T` con il tipo concreto richiesto (es. `max_int`), risolvendo le dipendenze in linguaggio C.

## Modifiche Apportate
### 1. Sistema dei Tipi e AST ([src/ast.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h), [src/ast_clone.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast_clone.h))
- Aggiunto `TYPE_META_TYPE` al sistema di enumerazione dei tipi, che rappresenta la parola chiave astratta [type](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h#499-507).
- Aggiunto `EXPR_TYPE` al gruppo delle espressioni per permettere a Lain di manipolare i tipi come veri e propri valori *arguments*.
- Implementato l'header [ast_clone.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast_clone.h) contentente la suite completa di routine per effettuare deep-copy immutabili dell'albero sintattico di una `DeclFunction`.

### 2. Parser ([src/parser/type.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/parser/type.h))
- Il parser valuta la parola chiave [type](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h#499-507) come un valido nome base del tipo che risolve nel meta-costrutto nativo [type_meta_type(arena)](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h#529-536).

### 3. Analizzatore Semantico e Sostituzione Generica
- Aggiunto il processore semantico per l'instanziazione ([src/sema/generic.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/generic.h)), un visitor del pattern dell'albero sintattico per rimpiazzare l'indicatore `T` con il concreto tipo base richiesto dal chiamante.
- Aggiornato [sema_resolve_expr](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/resolve.h#606-886) in [src/sema/resolve.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/resolve.h):
  - I nomi di tipo (es. [int](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast_print.h#68-74), [u8](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/typecheck.h#41-52), struct/enum custom definite dall'utente) quando parsati come `EXPR_IDENTIFIER` vengono risolti attivamente come passaggi parametro di valori tipo `EXPR_TYPE`.
  - Individuazione automatica delle chiamate `EXPR_CALL` a funzioni generiche ([comptime](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h#511-519)), mangling automatico basato sugli argomenti di tipo, estrazione degli argomenti, sostituzione T-per-Ttype nell'AST e pre-risoluzione.

### 4. Generatore di Codice C (`src/emit/...`)
- Ignorato in blocco l'output di signature contenenti `TYPE_META_TYPE` / `TYPE_COMPTIME` in [emit/decl.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/emit/decl.h). Le funzioni di "base" e template ora permangono solo nel meta-spazio dell'analizzatore semantico, evitando errori C.
- Introdotte Forward Declarations nel master emitter [emit.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/emit.h) per far fronte alla natura "aggiunta progressiva" delle funzioni generiche in ambiente `sema_decls`. La dichiarazione preventiva consente ai componenti reciprocamente ricorsivi di compilare.
- Cancellati passaggi di tipi parametro nelle chiamate a funzione C intercettando ed ignorando gli switch `EXPR_TYPE` dal loop di `emit_expr.h`.

## Validazione C e Test Eseguiti
Il file [tests/generics.ln](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/tests/generics.ln) è stato configurato e il runtime assicura che il C emesso sia conforme testando varie istanze e primitivi multi-tipo, incluse allocazioni di precisione e call con firme astratte:

```lain
proc print_int(val int) { libc_printf("%d\n", val) }
proc print_f32(val f32) { libc_printf("%f\n", val) }

proc max(comptime T type, a T, b T) T {
    if a > b { return a } return b
}
func identity(comptime T type, val T) T { return val }

proc main() {
    print_int(max(int, 10, 20))
    print_f32(max(f32, 3.14, 2.71))
    var x = identity(int, 42)
    print_int(x)
}
```
**Risultato del Test Run:** Tutti i test superati. L'uscita C [out.c](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/out.c) compila senza alcun errore di dichiarazione implicita o manipolazione errata dei pointer, producendo esecuzioni robuste e con type-safety garantita!

## Fase B: Type Aliases e Costrutti Anonimi (`type { ... }`)
L'obiettivo di questa fase era implementare l'interprete a tempo di compilazione (CTFE) in modo che le funzioni generiche [comptime](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h#511-519) potessero allocare e processare veri tipi custom (structs ed enum) senza far trasudare espressioni meta a livello di C (cioè senza emettere le implementazioni meta).

### 1. Nuovi Nodi AST e Parsing ([src/ast.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h), [src/parser/decl.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/parser/decl.h))
- Aggiunti i costrutti speciali `EXPR_ANON_STRUCT` e `EXPR_ANON_ENUM`, che permettono alle funzioni CTFE di costruire ed istanziare payload anonimi tramite clausole come `type { Some { value T } None }`.
- Integrato nel Parser il blocco `type Name = Expr` tramite `DECL_TYPE_ALIAS` che assegna un nome persistente ad un evaluation meta.

### 2. Interprete CTFE ([src/sema/comptime.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/comptime.h))
- Creato l'engine primario per l'esecuzione a tempo di compilazione: [comptime_evaluate_function](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/comptime.h#248-282) e [comptime_evaluate_expr](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/comptime.h#50-191).
- Introdotto un rudimentale `ComptimeEnv` locale per memorizzare gli argomenti delle funzioni.
- Aggiunto il supporto intrinseco a `compileError` (`@compileError`), in modo da poter forzare il fallimento della compilazione al fallire dell'asserzione di verità di un blocco [if](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/ast.h#722-730).
- Implementato l'handling semantico del blocco `STMT_IF` e delle operazioni `EXPR_BINARY` (`==` / `!=`) relative al controllo nativo di tipi.

### 3. Sincronizzazione in Analisi Semantica e C Code Emission
- In fase di risoluzione di un alias di tipo ([sema_resolve_expr](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/sema/resolve.h#606-886)), Lain processa in retroazione esplicitamente l'espressione generica eseguendola e convertendola in una top-level `DECL_STRUCT` o `DECL_ENUM`.
- Implementate protezioni all'interno del generatore di codice in C ([src/emit/decl.h](file:///home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain%20Compiler/lain/src/emit/decl.h)) che sopprimono definitivamente e correttamente la traduzione delle code block di funzioni la cui firma prevede o ritorna valori del metaframo puro (`TYPE_META_TYPE`).

### Risultati
Il backend C esegue e valida i test correttamente per la generazione CTFE del payload parametrizzato instradando le variazioni generiche senza compilarle a livello binario, confermando il superamento anche per l'asserzione tramite `compileError`.
