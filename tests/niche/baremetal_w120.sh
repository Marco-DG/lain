#!/usr/bin/env bash
# D-Niche M5: verify W120 fires on cortex-m4-bare (pointer pool=0).
# This is a shell helper invoked from the niche suite.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
out="$("$ROOT/lain" --target=cortex-m4-bare "$ROOT/tests/niche/option_pointer_pass.ln" 2>&1)"
if echo "$out" | grep -q "\[W120\]" && echo "$out" | grep -q "OptionNode"; then
    exit 0
fi
echo "W120 not emitted on bare-metal target for OptionNode"
echo "Output was:"
echo "$out"
exit 1
