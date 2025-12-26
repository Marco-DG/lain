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
run_test tests/func_proc.ln
run_test tests/mov_syntax.ln
run_test tests/ownership.ln
run_test tests/destructuring.ln
run_test tests/borrow_pass.ln
run_test tests/bounds_pass.ln

# Negative tests (should fail to compile)
run_negative_test() {
    FILE=$1
    echo "---------------------------------------------------"
    echo "Negative Test: $FILE (should fail)"
    ./src/compiler.exe $FILE 2>&1
    if [ $? -eq 0 ]; then
        echo "FAIL: $FILE should have failed compilation but succeeded"
        return 1
    else
        echo "PASS: $FILE correctly failed compilation"
        return 0
    fi
}

run_negative_test tests/purity_fail.ln
run_negative_test tests/borrow_conflict.ln
run_negative_test tests/bounds_fail.ln
