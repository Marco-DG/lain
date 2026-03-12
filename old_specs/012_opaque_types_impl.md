# Specification: Opaque Types & C Interop Implementation

## Overview
This document details the implementation of Opaque Types and Enhanced C Interoperability in the Lain compiler (v0.0.1). These features enable type-safe interaction with existing C libraries (specifically `libc`) without exposing internal C structures or compromising Lain's ownership model.

## 1. Opaque Types (`extern type`)

### Syntax
```lain
extern type Name;
```

### Semantics
*   **Definition**: Declares a type `Name` whose size and layout are unknown to Lain.
*   **Usage**: Can only be used behind a pointer (`*Name`). Attempting to instantiate it by value (e.g., `var x Name`) or determine its size (`sizeof(Name)`) is a compile-time error.
*   **C Emission**: Emits `typedef struct Name Name;` in the generated C code. This relies on C's incomplete type support, allowing pointers to the struct to be passed around even if the struct definition is not visible in the C output (assuming it's defined in an included header).

### Rationale
This allows wrapping C handles like `FILE`, `CURL`, or `SDL_Window` which are often exposed as pointers to undefined structs in public APIs.

## 2. C Type Mapping & Whitelisting

To ensure compatibility with C headers while maintaining Lain's strict type system, we implemented specific type mapping rules in the code emitter (`src/emit/decl.h`), particularly for standard library functions.

### The Mapping Problem
*   Lain uses `*u8` for byte strings. C uses `char *`.
*   Lain `*T` (Shared Reference) emits `const T*` by default. C APIs often take `T*` (mutable pointer) even for logically "const" operations (legacy APIs) or for handles that are modified internally (like `FILE*`).

### Implemented Solution
A whitelist-based override in `emit_decl` applies the following transformations for known function signatures (e.g., `fopen`, `fputs`, `fgets`, `printf`):

| Lain Type | Context | Emitted C Type | Reason |
| :--- | :--- | :--- | :--- |
| `*u8` | Parameter | `const char *` | Matches C string literals (const). |
| `var *u8` | Parameter | `char *` | Matches mutable C buffers. |
| `*FILE` | Parameter | `FILE *` | **CRITICAL**: Overrides default `const FILE*`. C's `FILE*` functions require non-const pointers even for reading/writing logic. |
| `mov *FILE` | Parameter | `FILE *` | Ownership transfer (Move) implies strictly owned pointer, mapped to value-type pointer `FILE*`. |
| `mov *FILE` | Return | `FILE *` | Factory functions (`fopen`) return an owned pointer. |

### Logic
The emitter checks the function name and parameter type. If it matches a known pattern (e.g., `fputs` taking `*FILE`), it bypasses the default recursive type emission and directly outputs the compatible C type string.

## 3. Standard Library: `std/fs`

Based on the above primitives, we implemented a safe `std/fs` module:

```lain
// std/fs.ln
type File {
    handle mov *FILE // Owned handle
}

func open_file(path *u8, mode *u8) File {
    return File(fopen(path, mode))
}

proc close_file(mov f File) {
    if f.handle != 0 {
        fclose(f.handle)
    }
}
```

### Ownership Model
*   **`File` Struct**: Owns the `FILE` handle.
*   **`mov f File`**: `close_file` consumes the `File` struct, ensuring the handle cannot be used after closing (Lineal Usage).
*   **Shared Access**: `write_file(f *File)` borrows the file. Internally it passes `f.handle` to `fputs`. Even though `f` is a shared reference, the underlying handle is a valid C pointer, and our whitelist ensures it is emitted as `FILE *` so `fputs` accepts it.

## 4. Future Requirements

To fully stabilize this feature set and expand the ecosystem, the following steps are necessary:

### 4.1. Automated Binding Generation
*   **Current State**: Bindings (like `std/c.ln`) are hand-written. White-listing names like `fgets` is brittle.
*   **Requirement**: A tool (bindgen) to parse C headers (using libclang) and generate:
    1.  `extern type` declarations.
    2.  `extern func` signatures with correct Lain types (`*u8` vs `*i8` vs `*char`).
    3.  `@c_map("char *")` annotations (future feature) so the compiler knows how to map types without hardcoded whitelists in `emit/decl.h`.

### 4.2. "Unsafe" C Interop Annotations
*   **Current State**: Pointers are assumed safe/valid if types match.
*   **Requirement**: Explicit usage of `unsafe` blocks for calling C functions that violate Lain's memory safety guarantees (e.g., `free`, `memcpy`). Currently `extern` functions are callable from safe code (except `unsafe` keyword on decl? No, currently implicitly unsafe). We should probably enforce `unsafe` for all `extern` calls unless marked `trusted`.

### 4.3. String Type Refinement
*   **Current State**: Strings are `*u8` (or slices). C strings are `char *`.
*   **Requirement**: A decided `c_string` or `cstring` type alias in stdlib that maps to `char *` universally, removing the need for `*u8` whitelisting.

### 4.4. Opaque Type Destructors
*   **Current State**: `File` must be manually closed or moved into `close_file`.
*   **Requirement**: Implicit destructors (RAII) for Opaque Types or Structs wrapping them, so `fclose` is called automatically when `File` goes out of scope.
