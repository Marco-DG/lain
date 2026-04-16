#!/bin/bash
set -e

# 1. Build the compiler
echo "[1/4] Building compiler..."
gcc src/main.c -o compiler -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -DARENA_DEBUG=1 -DMEMORY_PAGE_DEBUG=1 -DFILE_DEBUG=1

# 2. Run the compiler on the test file
echo "[2/4] Compiling tests/destructuring.ln..."
./compiler tests/destructuring.ln

# 3. Compile the generated C code
# We need -Isrc to find lain.h (which is now in the current dir, but -Isrc is good practice if we move it back)
# We need -Dtests_destructuring_printf=printf because the compiler mangles the extern printf
echo "[3/4] Compiling generated C code (out.c)..."
gcc out.c -o test_destruct -Isrc -Dtests_destructuring_printf=printf

# 4. Run the test executable
echo "[4/4] Running test..."
./test_destruct
