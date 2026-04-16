# Chapter 10 — Module System

## 10.1 Overview

Lain's module system provides source-level code organization through `import`
declarations. Modules map directly to source files using a dot-separated
naming convention. The module system operates at compile time — imports are
resolved and inlined during parsing, producing a single unified AST.

## 10.2 Import Declaration [Implemented]

### 10.2.1 Syntax

```
import_decl = "import" module_path ;
module_path = IDENTIFIER { "." IDENTIFIER } ;
```

### 10.2.2 Examples

```lain
import std.c              // imports std/c.ln
import std.io             // imports std/io.ln
import std.fs             // imports std/fs.ln
import mylib.utils        // imports mylib/utils.ln
```

### 10.2.3 Path Resolution

A module path maps to a filesystem path by replacing dots with directory
separators and appending `.ln`:

| Module Path | File Path |
|:------------|:----------|
| `std.c` | `std/c.ln` |
| `std.io` | `std/io.ln` |
| `std.fs` | `std/fs.ln` |
| `mylib.utils` | `mylib/utils.ln` |

Paths are resolved relative to the compiler's working directory.

## 10.3 Module Loading [Implemented]

### 10.3.1 Loading Process

When the compiler encounters an `import` declaration:

1. **Path construction**: The module path is converted to a file path.
2. **Deduplication check**: If the module has already been loaded, the import
   is skipped (no duplicate loading).
3. **File read**: The source file is read into the file arena.
4. **Lexing and parsing**: The file is lexed and parsed into an AST.
5. **Recursive resolution**: Any `import` declarations within the loaded
   module are recursively resolved.
6. **Splicing**: The loaded module's declarations are spliced into the AST
   in place of the `import` declaration.

### 10.3.2 Deduplication

Each module is loaded at most once. The module system maintains a linked
list of loaded modules indexed by their dot-separated name. Subsequent
imports of the same module are no-ops.

### 10.3.3 Module Record

Each loaded module is recorded with:

| Field | Description |
|:------|:------------|
| `name` | Dot-separated module path (e.g., `"std.io"`) |
| `decls` | The parsed AST declarations |
| `source_text` | Raw source text (retained for diagnostic display) |
| `source_file` | Filesystem path (for error messages) |

### 10.3.4 Circular Import Prevention

Since modules are deduplicated and loaded at most once, circular imports
are implicitly prevented. If module A imports module B, and module B imports
module A, the second import is a no-op because A is already loaded.

## 10.4 Name Resolution [Implemented]

### 10.4.1 Flat Namespace

After module inlining, all declarations from all modules share a single
global namespace. There is no module-qualified access syntax — all imported
names are directly available.

### 10.4.2 C Name Mangling

To prevent name collisions in the generated C code, all declarations are
prefixed with their module path:

| Lain Declaration | Generated C Name |
|:-----------------|:-----------------|
| `func add(...)` in `main.ln` | `main_add(...)` |
| `proc print(...)` in `std/io.ln` | `std_io_print(...)` |
| `type File` in `std/fs.ln` | `std_fs_File` |

The module path prefix uses underscores as separators (dots are replaced).

### 10.4.3 Name Lookup

Name resolution searches:
1. The current scope (local variables, parameters)
2. The global symbol table (all registered declarations across all modules)

There is no import aliasing or selective import — all public names from
an imported module are available.

## 10.5 Visibility [Implemented]

### 10.5.1 Current Model

All top-level declarations in a module are visible to importers. There is
no access control mechanism — all names are effectively public.

### 10.5.2 Export Keyword [Planned]

A future `export` keyword may restrict visibility:

```lain
export func public_api() int { ... }     // visible to importers
func internal_helper() int { ... }       // module-private
```

> Note: The `export` keyword is reserved but not yet implemented.

## 10.6 Standard Library Modules [Implemented]

The standard library is organized under the `std` namespace:

| Module | Purpose | Dependencies |
|:-------|:--------|:-------------|
| `std.c` | C standard library bindings | None |
| `std.io` | Console I/O (`print`, `println`) | `std.c` |
| `std.fs` | File system operations | `std.c` |
| `std.math` | Mathematical functions (`min`, `max`, `abs`, `clamp`) | None |
| `std.option` | Generic `Option` type | None |
| `std.result` | Generic `Result` type | None |
| `std.string` | String utilities | `std.c` (incomplete) |

### 10.6.1 std.c

Provides bindings to the C standard library via `c_include` and `extern`
declarations (see §11):

```lain
c_include "<stdio.h>"
c_include "<stdlib.h>"

extern type FILE
extern proc printf(fmt *u8, ...) int
extern proc fopen(filename *u8, mode *u8) mov *FILE
extern proc fclose(stream mov *FILE) int
```

### 10.6.2 std.io

Console output built on top of `std.c`:

```lain
import std.c

proc print(s u8[:0]) {
    libc_printf(s.data)
}

proc println(s u8[:0]) {
    libc_puts(s.data)
}
```

### 10.6.3 std.fs

File operations with ownership-safe handles:

```lain
import std.c

type File {
    mov handle *FILE       // linear field — File must be consumed
}

proc open_file(path u8[:0], mode u8[:0]) mov File {
    var raw = fopen(path.data, mode.data)
    return File(raw)
}

proc close_file(mov {handle} File) {
    fclose(mov handle)
}
```

### 10.6.4 std.math

Pure mathematical functions:

```lain
func min(a int, b int) int { ... }
func max(a int, b int) int { ... }
func abs(x int) int { ... }
func clamp(x int, lo int, hi int) int { ... }
```

### 10.6.5 std.option and std.result

Generic algebraic types (see §9):

```lain
func Option(comptime T type) type { ... }
func Result(comptime T type, comptime E type) type { ... }
```

## 10.7 Compilation Model [Implemented]

### 10.7.1 Unity Build

The Lain compiler uses a **unity build** model. All modules are parsed and
inlined into a single AST, which is then processed through the full semantic
analysis pipeline and emitted as a single C source file.

There is no separate compilation or linking of individual modules.

### 10.7.2 Compiler Passes

The compilation pipeline processes the unified AST:

1. **Parse & inline**: All modules are loaded, parsed, and spliced.
2. **Name resolution**: Global symbols are registered across all modules.
3. **Type checking**: Types are verified across the unified program.
4. **Ownership analysis**: Borrows and moves are checked globally.
5. **Code generation**: A single C file is emitted.

---

*This chapter is normative for implemented features. Sections marked [Planned]
describe future extensions and are not binding.*
