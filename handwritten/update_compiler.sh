#!/usr/bin/env bash
# Rebuilds the Lain compiler from source and copies the binary here.
# Run this after modifying the compiler source to get the updated version.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
DST="$SCRIPT_DIR/lain"

echo "Building Lain compiler..."
gcc -std=c99 -Wall -Wextra -o "$ROOT/lain" "$ROOT/src/main.c" -I "$ROOT/src"

cp -f "$ROOT/lain" "$DST"
chmod +x "$DST"

echo "OK: $DST updated"
"$DST" --version 2>/dev/null || true
