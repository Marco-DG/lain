# Chapter 15 — Standard Library

## 15.1 Overview

Lain's standard library is a collection of modules under the `std` namespace.
It provides essential functionality while remaining minimal — the library
favors small, composable building blocks over large frameworks.

All standard library modules are written in Lain itself (with C interop
for system-level operations). They are subject to the same ownership,
borrowing, and purity rules as user code.

## 15.2 Module Index

| Module | Purpose | Status |
|:-------|:--------|:-------|
| `std.c` | C standard library bindings | Implemented |
| `std.io` | Console I/O | Implemented |
| `std.fs` | File system operations | Implemented |
| `std.math` | Mathematical functions | Implemented |
| `std.option` | Generic `Option` type | Implemented |
| `std.result` | Generic `Result` type | Implemented |
| `std.string` | String utilities | Incomplete |

## 15.3 std.c — C Library Bindings [Implemented]

### 15.3.1 Purpose

Provides low-level bindings to the C standard library. This module is the
foundation for all other standard library modules that require system calls.

### 15.3.2 C Headers

```lain
c_include "<stdio.h>"
c_include "<stdlib.h>"
```

### 15.3.3 Opaque Types

```lain
extern type FILE
```

`FILE` is an opaque type representing a C standard I/O stream. It can only
be used through pointers and passed to the extern functions declared below.

### 15.3.4 Extern Declarations

| Declaration | Signature | Ownership |
|:------------|:----------|:----------|
| `printf` | `extern proc printf(fmt *u8, ...) int` | Shared args |
| `fopen` | `extern proc fopen(filename *u8, mode *u8) mov *FILE` | Returns owned handle |
| `fclose` | `extern proc fclose(stream mov *FILE) int` | Consumes handle |
| `fputs` | `extern proc fputs(s *u8, stream *FILE) int` | Shared args |
| `fgets` | `extern proc fgets(s var *u8, n int, stream *FILE) var *u8` | Mutable buffer |
| `libc_printf` | `extern proc libc_printf(fmt *u8, ...) int` | Shared args |
| `libc_puts` | `extern proc libc_puts(s *u8) int` | Shared args |

> Note: `libc_printf` and `libc_puts` are aliases that avoid name conflicts
> with user-defined functions.

## 15.4 std.io — Console I/O [Implemented]

### 15.4.1 Purpose

Provides simple console output operations.

### 15.4.2 Dependencies

```lain
import std.c
```

### 15.4.3 Functions

#### `print`

```lain
proc print(s u8[:0])
```

Prints a null-terminated string to standard output without a trailing newline.

**Parameters:**
- `s` — Shared borrow of a null-terminated byte slice.

#### `println`

```lain
proc println(s u8[:0])
```

Prints a null-terminated string to standard output followed by a newline.

**Parameters:**
- `s` — Shared borrow of a null-terminated byte slice.

## 15.5 std.fs — File System [Implemented]

### 15.5.1 Purpose

Provides ownership-safe file operations built on top of C's `stdio.h`.

### 15.5.2 Dependencies

```lain
import std.c
```

### 15.5.3 Types

#### `File`

```lain
type File {
    mov handle *FILE
}
```

A linear type wrapping a C `FILE*` handle. Because the `handle` field is
annotated with `mov`, the `File` type is linear — it must be consumed
exactly once (typically by calling `close_file`).

### 15.5.4 Functions

#### `open_file`

```lain
proc open_file(path u8[:0], mode u8[:0]) mov File
```

Opens a file and returns an owned `File` handle.

**Parameters:**
- `path` — File path as a null-terminated string.
- `mode` — Open mode (`"r"`, `"w"`, `"a"`, etc.).

**Returns:** An owned `File` value. The caller is responsible for closing it.

#### `close_file`

```lain
proc close_file(mov {handle} File)
```

Closes a file, consuming the `File` value. Uses destructured parameter
syntax to extract and close the underlying C handle.

**Parameters:**
- Consumes the `File` value (ownership transfer via `mov`).

#### `write_file`

```lain
proc write_file(f File, s u8[:0])
```

Writes a string to a file.

**Parameters:**
- `f` — Shared borrow of the file handle.
- `s` — Shared borrow of the string to write.

### 15.5.5 Usage Example

```lain
import std.fs

proc main() {
    var f = open_file("output.txt", "w")
    write_file(f, "Hello, world!\n")
    close_file(mov f)
}
```

With `defer` for automatic cleanup:

```lain
proc main() {
    var f = open_file("output.txt", "w")
    defer { close_file(mov f) }
    write_file(f, "Hello, world!\n")
    // f is automatically closed on scope exit
}
```

## 15.6 std.math — Mathematical Functions [Implemented]

### 15.6.1 Purpose

Pure mathematical utility functions.

### 15.6.2 Dependencies

None.

### 15.6.3 Functions

All functions in `std.math` are `func` (pure) — they are guaranteed to
terminate, have no side effects, and can be called from any context.

#### `min`

```lain
func min(a int, b int) int
```

Returns the smaller of `a` and `b`.

#### `max`

```lain
func max(a int, b int) int
```

Returns the larger of `a` and `b`.

#### `abs`

```lain
func abs(x int) int
```

Returns the absolute value of `x`.

#### `clamp`

```lain
func clamp(x int, lo int, hi int) int
```

Returns `x` clamped to the range `[lo, hi]`. If `x < lo`, returns `lo`.
If `x > hi`, returns `hi`. Otherwise returns `x`.

## 15.7 std.option — Generic Option Type [Implemented]

### 15.7.1 Purpose

Provides a generic `Option` type for representing optional values.

### 15.7.2 Dependencies

None.

### 15.7.3 Type Constructor

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}
```

### 15.7.4 Instantiation

```lain
type OptionInt = Option(int)
type OptionFile = Option(File)
```

### 15.7.5 Variants

| Variant | Fields | Meaning |
|:--------|:-------|:--------|
| `Some` | `value T` | A value is present |
| `None` | (none) | No value |

### 15.7.6 Usage

```lain
type OptionInt = Option(int)

func find(arr int[10], target int) OptionInt {
    for i in 0..10 {
        if arr[i] == target {
            return OptionInt.Some(target)
        }
    }
    return OptionInt.None
}

proc main() {
    var arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    var result = find(arr, 5)
    case result {
        Some(v): libc_printf("Found: %d\n", v)
        None: libc_printf("Not found\n")
    }
}
```

## 15.8 std.result — Generic Result Type [Implemented]

### 15.8.1 Purpose

Provides a generic `Result` type for representing computations that may
succeed or fail.

### 15.8.2 Dependencies

None.

### 15.8.3 Type Constructor

```lain
func Result(comptime T type, comptime E type) type {
    return type {
        Ok { value T }
        Err { error E }
    }
}
```

### 15.8.4 Instantiation

```lain
type ResultInt = Result(int, int)
type FileResult = Result(File, int)
```

### 15.8.5 Variants

| Variant | Fields | Meaning |
|:--------|:-------|:--------|
| `Ok` | `value T` | Operation succeeded |
| `Err` | `error E` | Operation failed |

### 15.8.6 Usage

```lain
type FileResult = Result(File, int)

proc try_open(path u8[:0]) FileResult {
    var raw = fopen(path.data, "r")
    if raw == 0 {
        return FileResult.Err(1)
    }
    return FileResult.Ok(File(raw))
}

proc main() {
    var result = try_open("data.txt")
    case result {
        Ok(f): {
            write_file(f, "hello")
            close_file(mov f)
        }
        Err(code): libc_printf("Error: %d\n", code)
    }
}
```

## 15.9 std.string — String Utilities [Incomplete]

### 15.9.1 Status

The string module is currently incomplete due to the `u8` vs `char` type
mismatch (see §11.8.3). Extern declarations of C string functions
(`strlen`, `strcmp`, etc.) cannot be used because Lain emits `uint8_t*`
but C expects `char*`.

### 15.9.2 Planned Functions

| Function | Signature | Description |
|:---------|:----------|:------------|
| `strlen` | `func strlen(s u8[:0]) int` | String length |
| `strcmp` | `func strcmp(a u8[:0], b u8[:0]) int` | String comparison |
| `strcpy` | `proc strcpy(var dst *u8, src *u8)` | String copy |

These will be implemented once the char interop issue is resolved.

## 15.10 Planned Standard Library Modules

| Module | Purpose | Priority |
|:-------|:--------|:---------|
| `std.mem` | Memory allocation utilities | High |
| `std.collections` | Dynamic array, hash map | Medium |
| `std.fmt` | String formatting | Medium |
| `std.os` | OS-level operations | Low |

---

*This chapter is normative for implemented modules. Planned modules are
described for informational purposes only.*
