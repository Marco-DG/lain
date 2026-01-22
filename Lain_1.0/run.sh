#!/bin/bash
set -e

# Run the Lain Compiler (from parent dir)
# Assumes we are in Lain_1.0 directory
if [ ! -f "../src/compiler.exe" ]; then
    echo "Error: ../src/compiler.exe not found. Build it first."
    exit 1
fi

echo "--- Compiling Lain 1.0 ---"
../src/compiler.exe src/main.ln

if [ -f "out.c" ]; then
    echo "--- Generated C Code (saved to generated.c) ---"
    cp out.c generated.c
    # Optional: cat generated.c | head -n 20
else
    echo "Error: out.c not produced."
    exit 1
fi

echo "--- Compiling with GCC ---"
gcc -g -o lain_app out.c

echo "--- Running ---"
./lain_app
