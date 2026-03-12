# Zero-Cost Error Handling: From Nulls to Niches

## The Optimization Purist's Goal
You correctly identified that `NULL` is computationally efficient because it allows validity checking with a single CPU instruction (`CMP` or `TEST`), without the overhead of wrapping structs or auxiliary boolean flags.

Traditional "Safe" languages often implement `Option<T>` as a tagged union:
```c
struct Option_File {
    bool has_value; // +1 byte (often padded to 8)
    File payload;   // +8 bytes (pointer)
}; // Total: 16 bytes! Double the size of a pointer.
```
This is unacceptable for an optimization purist. We want the safety of Types but the speed of Assembly.

## Solution 1: Niche Optimization (The "Smart Null")
We can make `Option<File>` have the **exact same size and runtime cost** as a raw pointer.

### How it works
The compiler analyzes the memory layout of the type `T`.
- A pointer `*T` has a **"Niche"**: the value `0x0` is physically impossible for a valid reference.
- The `Option` type can "inhabit" this niche to represent `None`.

| Logical Type | Physical Representation (64-bit) | Cost |
| :--- | :--- | :--- |
| `File` | `0x0000_0000_1000_0000` (Valid Address) | 1 register |
| `Option<File>` (`Some`) | `0x0000_0000_1000_0000` (Same Address) | 1 register |
| `Option<File>` (`None`) | `0x0000_0000_0000_0000` (Zero) | 1 register |

**Runtime check:**
```asm
cmp rax, 0
je  handle_none
```
This is **identical** to C's `if (ptr == NULL)`.
**Difference**: In Lain, you cannot *accidentally* dereference it without checking. The compiler forces the check, but the CPU executes the same optimal code.

## Solution 2: "Multiple Nulls" (The User's Insight)
You mentioned: *"There are multiple invalid addresses".*
This is a brilliant insight often used in high-performance Kernels (like Linux) but rarely in user-space languages. It allows encoding **Result types** (Value OR Error) in a single pointer.

### The Address Space Holes
On 64-bit systems (x86_64), virtual addresses are 48-bit or 57-bit. This leaves vast ranges of "Canonical Encodings" unused, or reserved for Kernel.
Specifically, the range `0xFFFFFFFFFFFFF001` to `0xFFFFFFFFFFFFFFFF` (the very top of memory) is effectively impossible for a valid user-space object allocation.

### The optimization: `ERR_PTR`
We can implement `Result<File, ErrorCode>` as a single 64-bit value:

1.  **Valid**: `0x0000...` to `0x7FFF...` (User space pointer)
2.  **NULL**: `0x0000_0000_0000_0000` (Optional "Hard Null")
3.  **Error N**: `0xFFFFFFFFFFFFFFFF` minus `N`.

**Encoding `Result<File, Error>`:**
```c
// Pseudo-implementation
void* result_encoding(void* ptr, int error) {
    if (error) return (void*)( -error ); // e.g. -2 for ENOENT -> 0xFF...FE
    return ptr;
}
```

**Decoding (Zero Cost Check):**
```asm
; Check validity
cmp rax, -4095  ; defined max error range
ja  is_error    ; Unsigned comparison: high values jump
; else, it is a valid pointer
```

This gives you **Rich Error Types** (FileNotFound, PermissionDenied, etc.) for the **exact same cost** as checking for NULL.

## Proposed Plan for Lain

1.  **Phase 1: Implement Niche Optimization for Enums**
    - Modify `Type` system to track "invalid bit patterns" for types.
    - Pointers declare `0` as invalid.
    - When `Option<T>` is instantiated with a niche-having `T`, eliminate the tag field.
    
2.  **Phase 2: The `PointerResult` Optimization**
    - Special handling for `Result<*T, E>` where `E` is a small enum/integer.
    - Map `E` variants to the top of the address space.
    - This realizes your vision of "Multiple Nulls".

## Immediate Action for `std/fs`
For `open_file`, we simply need to wrap the return type.
Since C returns `NULL` on error, and sets `errno` for the specific error:

```lain
// std/fs.ln changes
import std.c
import std.os // for errno

type Option<T> enum {
    None,
    Some(T)
}

func open_file(path u8[:0], mode u8[:0]) mov Option<File> {
    var raw = fopen(path.data, mode.data)
    if raw == 0 {
        return Option.None
    }
    return Option.Some(File(raw))
}
```

If we implement Niche Optimization, `Option<File>` will have size 8 bytes.
If valid: holds pointer.
If None: holds 0.

This satisfies your requirement: **Zero computational cost over C, but safe.**
