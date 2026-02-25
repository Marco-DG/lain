#!/bin/bash
set -e
set -x

# Run the Lain Compiler from the parent directory
echo "--- Compiling Lain 1.0 Lexer ---"

cd ..
if [ ! -f "compiler.exe" ]; then
    echo "Error: compiler.exe not found in parent directory. Build it first."
    exit 1
fi

./compiler.exe Lain_1/src/main.ln

echo "--- Generated C Code (saved to generated.c) ---"
cp -f out.c Lain_1/generated.c
mv -f out.c Lain_1/out.c
cp -f lain.h Lain_1/lain.h

cd Lain_1

echo "--- Compiling with Cosmocc ---"
../cosmocc/bin/cosmocc out.c -o lain_app -w -Wno-pointer-sign -Dlibc_printf=printf -Dlibc_puts=puts

echo "--- Running ---"
./lain_app
