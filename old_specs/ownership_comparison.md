# Analisi Comparativa: Gestione dell'Ownership e Tipi Lineari

Questo documento offre un confronto estensivo e dettagliato tra il modello di gestione della memoria e dell'ownership implementato in **Lain**, e gli approcci adottati da **Rust** e **C++**. L'obiettivo è evidenziare le scelte di design alla base di Lain (focalizzate sull'esplicitezza) e contestualizzarle nel panorama dei linguaggi di programmazione di sistema moderni.

---

## 1. Filosofia di Base dei Tre Linguaggi

La gestione sicura delle risorse (memoria, file handle, socket, ecc.) senza l'uso di un Garbage Collector è uno dei problemi più complessi nel design dei linguaggi.

*   **C++ (L'Approccio "Opt-In" tramite Costruttori):** C++ nasce con una semantica di copia predefinita (tutto viene copiato di default). Con il C++11 sono state introdotte le *move semantics* tramite `std::move` e le rvalue references (`&&`). Il programmatore può disabilitare la copia eliminando i costruttori di copia (`= delete`), ma la sicurezza contro difetti come l'*use-after-move* è delegata principalmente all'analisi statica esterna (es. clang-tidy) o alla disciplina dello sviluppatore (lo standard definisce lo stato "moved-from" come "valido ma non specificato").
*   **Rust (L'Approccio "Move by Default" Implicito):** Rust ribalta il paradigma del C++: tutto è *move-only* di default (tipi affini), a meno che un tipo non implementi esplicitamente il trait `Copy`. Rust garantisce la sicurezza della memoria al 100% a compile-time tramite il suo Borrow Checker. Tuttavia, i trasferimenti di ownership (move) sono **impliciti** al sito di chiamata: passare una variabile a una funzione consuma silenziosamente la variabile stessa.
*   **Lain (L'Approccio della "Esplicitezza Totale"):** Lain adotta un modello ispirato alla teoria dei tipi lineari puro, unito a un Borrow Checker come Rust, ma impone un requisito fondamentale: **niente "magia" implicita**. Se c'è un trasferimento di possesso o se un dato è limitato a un'unica locazione (move-only), ciò deve essere marcato sintatticamente sia nella definizione dei dati (es. `own`/`mov` nelle `struct`) sia nel sito in cui avviene il trasferimento (operazioni di chiamata e di ritorno).

---

## 2. Dichiarazione di Tipi "Move-Only" (Linearità/Affinità)

Come si dice al compilatore: *"Questa struttura dati possiede una risorsa unica e non può essere banalmente clonata byte-per-byte"*?

### C++
In C++, devi esplicitamente disabilitare le operazioni di copia. È un meccanismo basato su metodi speciali della classe.

```cpp
class Handle {
    int* ptr;
public:
    Handle(int* p) : ptr(p) {}
    ~Handle() { delete ptr; }

    // Disabilita la copia
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    // Abilita esplicitamente il move
    Handle(Handle&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr; // Lo stato moved-from deve essere gestito a mano
    }
    Handle& operator=(Handle&& other) noexcept { /* ... */ return *this; }
};

class Wrapper {
    Handle h; // Wrapper diventa automaticamente un-copyable perché Handle lo è
};
```

### Rust
In Rust, l'ownership e l'essere *move-only* è il comportamento standard. Qualsiasi `struct` è affine, a meno che tu non la marchi con il trait `Copy`.

```rust
struct Handle {
    ptr: Box<i32> // Box è un tipo affine fornito dalla stdlib
}

// Wrapper è automaticamente affine perché contiene Handle, che è affine.
// Non serve nessuna keyword.
struct Wrapper {
    h: Handle
}
```

### Lain
Lain richiede che l'esclusività (linearità) di una risorsa sia **documentata nel sistema di tipi** in maniera diretta e visibile sulle singole proprietà. Aggiungendo una keyword (come `own`, `uni` o `mov`) a un campo di una struct, l'intera struct diventa lineare e soggetta all'analisi del Borrow Checker come tipo un-copyable.

```lain
type Handle {
    own ptr *int  // La struct 'possiede in esclusiva' (own) questo puntatore
}

type Wrapper {
    h Handle     // Transitivamente lineare
}
```
**Confronto:** C++ richiede un sacco di codice boilerplate (la Rule of 5). Rust è estremamente ergonomico ma nasconde l'informazione ("Devi sapere se `Box<i32>` implementa o meno `Copy` per capire che `Handle` è move-only"). Lain offre un ottimo bilanciamento: è dichiarativo ed esplicito senza boilerplate.

---

## 3. Trasferimento di Ownership (Call-Site / Move)

Cosa succede quando cedi l'ownership di una variabile a una funzione?

### C++
Il trasferimento richiede il casting a rvalue-reference tramite `std::move`.

```cpp
void take_ownership(Handle val) {
    // val viene distrutto a fine scope
}

int main() {
    Handle h(new int(42));
    take_ownership(std::move(h)); // Il programmatore dichiara esplicitamente il move
    
    // ATTENZIONE: Il compilatore permette questa riga!
    // 'h' è in uno stato "valido ma non specificato". Segfault o UB molto probabile in runtime.
    // int x = *h.ptr; 
}
```

### Rust
Rust non usa keyword al sito di chiamata. Il *move* è dedotto dalla firma della funzione (se accetta `T` e non `&T`). Il compilatore impedisce l'use-after-move staticamente.

```rust
fn take_ownership(val: Handle) {
    // val viene liberata qui (Drop)
}

fn main() {
    let h = Handle { ptr: Box::new(42) };
    
    take_ownership(h); // IL MOVE È IMPLICITO. Visivamente sembra una normale chiamata.
    
    // print(h.ptr); // ERRORE DI COMPILAZIONE (Borrok Checker): use after move
}
```

### Lain
Lain unisce la sicurezza a compile-time di Rust con l'esplicitezza sintattica del C++. Al momento della chiamata, l'utente *deve* utilizzare una keyword (come `mov`, `move` o `give`) per indicare al compilatore e a chi legge il codice che l'ownership sta per essere ceduta irreversibilmente.

```lain
func take_ownership(own val Handle) {
    // val muore qui se non ritornato
}

proc main() int {
    var h = Handle(ptr)
    
    take_ownership(move h) // L'intento distruttivo è ESPLICITO.
    
    // var x = h // ERRORE DI COMPILAZIONE (Borrow Checker Lain): use of linear variable h
    
    return 0
}
```
**Confronto:** Rust sacrifica la leggibilità locale (il lettore deve saltare alla definizione di `take_ownership` per capire se `h` sopravviverà o meno al sito della chiamata) in favore di una grammatica pulita, punendo l'errore a posteriori. In Lain, il programmatore codifica *fisicamente* l'intento di perdere l'ownership e documenta l'operazione. È una difesa proattiva.

---

## 4. Ritornare Tipi Lineari (Return Exp)

Quando si crea o si trasferisce una risorsa verso lo scope chiamante (dal callee al caller).

### C++
Grazie a feature come (N)RVO (Named Return Value Optimization), C++ elide automaticamente le copie, o si riduce a chiamare il costruttore di "move" in maniera implicita. A volte si usa (sbagliando, ostacolando la RVO) `return std::move(x);`.
```cpp
Handle create() {
    Handle h(new int(10));
    return h; // Move implicito / Elisione
}
```

### Rust
Trasparente, come il call-site. Un valore lineare viene semplicemente restituito.
```rust
fn create() -> Handle {
    let h = Handle { ptr: Box::new(10) };
    h // Move implicito
}
```

### Lain
Coerentemente con la filosofia di esplicitezza in entrambe le direzioni, Lain forza a qualificare anche il ritorno di risorse gestite.
```lain
func create() Handle {
    var h = Handle(ptr)
    return move h // La keyword esplicita sottolinea il trasferimento di risorse out-bound
}
```

---

## 5. Il Borrowing (Passaggio per Riferimento)

Quando vogliamo far "ispezionare" un dato senza cederne l'ownership.

*   **C++**: `void inspect(const Handle& h)` (oppure puntatore `Handle*`). Libera proliferazione, rischio altissimo di *"Dangling References"* e referenze "invalidate" se il proprietario originario muore. Non controllato a compile-time.
*   **Rust**: `fn inspect(h: &Handle)`. Regolato dal *Borrow Checker*. Rigide regole ("Aliasing XOR Mutability"): puoi avere infiniti riferimenti immutabili, o uno solo mutabile, mai contemporaneamente. Assicura assenza di Data Races e riferimenti pendenti.
*   **Lain**: Simile a Rust, con Borrow Checker integrato. In Lain, si usa una keyword di *borrowing* per prestare un valore in maniera sicura senza infrangere la linearità. Grazie alle verifiche NLL (Non-Lexical Lifetimes) e ai constraint, si assicura che il borrow non sopravviva all'owner.

---

## 6. Pro e Contro dell'Approccio Lain

La scelta adottata in Lain porta precise e determinanti conseguenze nell'ecosistema del linguaggio.

### I Vantaggi Assoluti (Perché Lain fa bene)
1. **Curva di Apprendimento "Gentile" della Sicurezza:** In Rust è noto lo scoglio dei "Move" impliciti e dei "Borrow Errors" imprevisti. Un novizio passa `String` a una funzione e si sorprende di non poterla usare dopo. In Lain, inserendo keyword obbligatorie al sito di chiamata (es. `move h`), il programmatore comprende la natura *distruttiva* dell'operazione ancor prima di compilare.
2. **Readability senza IDE:** Leggere codice Lain su GitHub o su un terminale, senza un Language Server (LSP) o hover-tips del passaggio del mouse, permette di dedurre il flusso di memoria alla perfezione. In Rust o C++, senza l'IDE che sottolinea o spiega, bisogna tenere in mente enormi grafi mentali di chi fa cosa.
3. **Data-Oriented Modeling:** Definire `own ptr *int` chiarisce *subito* che la struct gestisce qualcosa in esclusiva, contrariamente a Rust in cui devi sapere a memoria quali std-types (File, Box, Rc) o sub-struct del tuo progetto non fittano in un banale struct-clone.

### I (Pochi) Compromessi
1. **Verbosità Sintattica:** In codice matematico o pesantemente generico in cui vi sono decine di trasferimenti innocui di dati, il programmatore si troverà a digitare costantemente `move this` o `own that`. È il prezzo da pagare per l'esplicitezza totale. *È un eccellente trade-off.*
2. **Inerzia per gli Esperti:** Chi viene da uno sfondo Rust potrebbe inizialmente provare noia nel definire keyword di "move" anche quando il flow "Rust-like" sarebbe sufficiente al compilatore per inferire la transazione. Ma, di nuovo, i benefici di leggibilità ripagano la fatica di stesura (si passa molto più tempo a *leggere* codice che a scriverlo).

## Conclusioni
Il design dei tipi lineari di Lain è eccellente e moderno. Si pone esattamente a **metà strada** (prendendo il meglio di entrambi i mondi) tra la pericolosa manipolazione esplicita del C++ (`std::move`, ma con use-after-move possibile) e la sicura manipolazione implicita di Rust (nessun bug, ma invisibilità causale del consumo dei dati alle chiamate).

Lain implementa **"Esplicitezza Visiva & Formalità Compilativa"**: obbliga allo statement visuale, ed applica i freni di sicurezza matematici di un vero Borrow Checker sulle operazioni.
