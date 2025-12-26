#!/bin/bash

# Build compiler
gcc src/main.c -o src/compiler.exe -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -DARENA_DEBUG=1
if [ $? -ne 0 ]; then
    echo "Failed to build compiler"
    exit 1
fi

# Function to run a test
run_test() {
    FILE=$1
    echo "---------------------------------------------------"
    echo "Testing $FILE"
    ./src/compiler.exe $FILE
    if [ $? -ne 0 ]; then
        echo "Compiler failed for $FILE"
        return
    fi
    
    # Compile generated C code
    # Assuming cosmocc is at cosmocc/bin/cosmocc
    ./cosmocc/bin/cosmocc out.c -o out/test.exe -w
    if [ $? -ne 0 ]; then
        echo "C compilation failed for $FILE"
        return
    fi
    
    # Run executable
    ./out/test.exe
    echo ""
}

run_test tests/control_flow.ln
run_test tests/functions.ln
run_test tests/structs.ln
run_test tests/arrays.ln
run_test tests/math.ln
