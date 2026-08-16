#!/usr/bin/env bash
# Usage: bash update_compiler.sh  (NOT sh — requires bash for ${BASH_SOURCE[0]})
# Copies the main Lain compiler binary to this directory.
# Run this after modifying the compiler source to get the updated version.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/../.."
DST="$SCRIPT_DIR/lain"
SRC="$PROJECT_ROOT/lain"

if [ ! -f "$SRC" ]; then
    echo "Error: Lain compiler not found at $SRC"
    echo "Build the compiler first."
    exit 1
fi

echo "Copying Lain compiler from $SRC..."

cp -f "$SRC" "$DST"
chmod +x "$DST"

echo "OK: $DST updated"
"$DST" --version 2>/dev/null || true
