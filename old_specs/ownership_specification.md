# Specifiche di Ownership e Borrowing in Lain

Questo documento illustra il meccanismo alla base dell'ownership e del borrowing in **Lain**, desunto esclusivamente dal capitolo ufficiale 07 "Ownership & Borrowing". Niente astrazioni superflue: Lain controlla la correttezza a compile-time (tramite il Borrow Checker) ed è esplicito in ogni passaggio.

---

## 1. Le Tre Modalità Base

In Lain, ogni risorsa può essere passata in tre modi ben precisi. È fondamentale notare come la sintassi richiesta a livello di API (firma della funzione) debba essere *esattamente* rispecchiata al call-site da chi chiama la funzione.

| Costrutto | Sintassi Parametro | Sintassi Chiamata | Semantica |
| :--- | :--- | :--- | :--- |
| **Shared** | `func f(x T)` | `f(x)` | **Prestito Immutabile**. Permette multipli borrow in sola lettura simultaneamente. |
| **Mutable** | `func f(var x T)` | `f(var x)` | **Prestito Mutabile** (Esclusivo). Permette lettura e scrittura, ma blocca ogni altro accesso. |
| **Owned** | `func f(mov x T)` | `f(mov x)` | **Spostamento e Consumo (Move)**. La proprietà viene ceduta. Variabile sorgente viene *invalidata*. |

*Lain non usa keyword come `own`, `borrow`, `mut borrow` o suffissi `!`*. Le uniche variazioni sintattiche sono `var` (per la mutabilità ai riferimenti) e `mov` (per l'ownership). Il default senza keyword indica un prestito condiviso (Shared).

---

## 2. Tipi Lineari (Resource)
Un tipo è considerato "lineare" (non può essere copiato, e deve essere consumato *esattamente* una sola volta per prevenire memory leak o resource drop impliciti) se:
1. Contiene un campo la cui dichiarazione è annotata con `mov`, oppure
2. Contiene a sua volta un tipo che è lineare (transitività).

```lain
// 'mov' sul campo handle rende implicitamente la struct File lineare.
type File {
    mov handle *int 
}
```

Una variabile lineare che esce dallo scope senza essere consumata produce l'errore `[E002]`.

---

## 3. Definizione di API (Firme delle Funzioni)
Le firme in Lain sono chiare e indicano cosa deve succedere alla variabile.

```lain
// Prende l'ownership e la consuma.
func closeFile(mov file File)

// Prende un borrow mutabile (altera, ma non consuma).
func writeString(var file File, content string)

// Ritorna la proprietà al chiamante.
func openFile(path string) File {
    var f = ...
    return mov f // il ritorno come owner cede esplicitamente la risorsa.
}
```

---

## 4. Uso Rigoroso al Call-Site
Questa è l'arma in più della filosofia "Strict" di Lain. Il compilatore non ti lascia inferire: se cedi ownership o mutabilità, *devi dichiararlo quando chiami la funzione*.

```lain
var f = openFile("data.txt")

// 1. Prestito esclusivo mutabile: 
// Segnalo a chi legge il codice che `f` sta per subire mutazioni.
writeString(var f, "Hello Lain")

// 2. Prestito shared
var text = readFile(f)

// 3. Move/Consumo
// Segnalo e forzo la consapevolezza che 'f' d'ora in poi è inaccessibile.
closeFile(mov f)

// [E001] "Use after move" o [E003] "Double move"
// closeFile(mov f)
// writeString(var f, "Ouch")
```

---

## 5. Il Borrow Checker (Lifetimes & Mutability)

Lain si previene dai data race e dalle eccezioni implementando un Borrow Checker robusto (ispirato al paradigma Read-Write Lock):
* Alla stessa variabile si possono fare illimitati accessi `Shared` oppure **solo un** accesso `Mutable`.
* I prestiti usano **Non-Lexical Lifetimes (NLL)**: il prestito di una variabile spira all'ultimo reale utilizzo (last use) del riferimento, non necessariamente alla fine del blocco parentesi.
* Supporto al **Two-Phase Borrow**: un prestito mutabile scatta davvero solo nell'argomento, permettendo cose come `v.push_n(v.cap)`.

Tutto a tolleranza zero: nessun runtime overhead, tutto controllato in static analysis.
