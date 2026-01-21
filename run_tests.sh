#!/bin/bash

# Build compiler
echo "Building Compiler..."
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
        exit 1
    fi
    
    # Compile generated C code
    # Assuming cosmocc is at cosmocc/bin/cosmocc
    ./cosmocc/bin/cosmocc out.c -o out/test.exe -w
    if [ $? -ne 0 ]; then
        echo "C compilation failed for $FILE"
        exit 1
    fi
    
    # Run executable
    ./out/test.exe
    echo ""
}

# Function to run a negative test
run_negative_test() {
    FILE=$1
    echo "---------------------------------------------------"
    echo "Negative Test: $FILE (should fail)"
    ./src/compiler.exe $FILE 2>&1
    if [ $? -eq 0 ]; then
        echo "FAIL: $FILE should have failed compilation but succeeded"
        exit 1
    else
        echo "PASS: $FILE correctly failed compilation"
        return 0
    fi
}

if [ "$1" != "" ]; then
    if [[ "$1" == *"_fail.ln" ]]; then
        run_negative_test "$1"
    else
        run_test "$1"
    fi
    echo "---------------------------------------------------"
    echo "Single test execution complete."
    exit 0
fi

echo "=== Running Positive Tests ==="
# Find all .ln files that do NOT contain "_fail" in their name
# Sort them for consistency
# Using process substitution or simply listing files to avoid subshell exit issue
for test_file in $(find tests -name "*.ln" ! -name "*_fail*" ! -name "syntax_check.ln" | sort); do
    run_test "$test_file"
done

echo ""
echo "=== Running Negative Tests ==="
# Find all .ln files that DO contain "_fail" in their name
for test_file in $(find tests -name "*_fail.ln" | sort); do
    run_negative_test "$test_file"
done

echo "---------------------------------------------------"
echo "All tests passed!"
