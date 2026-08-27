#!/usr/bin/env bash
# fuzz_annot.sh — teeth for const/pure ANNOTATION soundness (the beyond-C thesis's
# codegen annotations). A `func` annotated __attribute__((const/pure)) whose call
# result is UNUSED can be ELIDED by gcc — which is UNSOUND if the func has an
# observable effect (a `panic`/abort). This fuzzer generates funcs with randomized
# panic/call/pure patterns, calls them with results used AND unused, and compiles
# each accepted program at -O0 AND -O3: if the two outputs DIFFER, an annotation let
# gcc drop an observable effect at one level — a miscompile. (This is the class of
# the confirmed `const` on a panicking func dropping its abort at -O0.)
set -u
cd "$(dirname "$0")"
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_annot.$$"; mkdir -p "$SC"
N="${1:-250}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; miscompiles=0; brokenc=0

for ((i=0; i<N; i++)); do
    shape=$((RANDOM % 4))
    thr=$((RANDOM % 5))               # panic threshold
    arg=$(( (RANDOM % 8) - 3 ))       # call argument (may cross threshold)
    used=$((RANDOM % 2))              # is the result used?
    case $shape in
      0) fbody="    if x < $thr {
        panic(\"low\")
    }
    return x";;                                            # may panic
      1) fbody="    return x + 1";;                        # pure
      2) fbody="    if x < $thr {
        panic(\"low\")
    }
    return x * 2";;                                        # may panic + mul
      3) fbody="    return x";;                            # trivial pure
    esac
    src="$SC/t_$i.ln"
    {
      echo 'extern proc libc_printf(fmt *u8, ...) i32'
      echo "func f(x i32) i32 {"
      echo "$fbody"
      echo "}"
      echo 'proc main() i32 {'
      if [ $used -eq 1 ]; then
        echo "    var r i32 = f($arg)"
        echo '    libc_printf("r=%d\n", r)'
      else
        echo "    f($arg)"
        echo '    libc_printf("after\n")'
      fi
      echo '    return 0'
      echo '}'
    } > "$src"
    cfile="$SC/t_$i.c"
    if ! $LAIN "$src" -o "$cfile" >/dev/null 2>&1; then rejected=$((rejected+1)); continue; fi
    accepted=$((accepted+1))
    o0="$SC/o0_$i"; o3="$SC/o3_$i"
    gcc -O0 -o "$o0" "$cfile" -Dlibc_printf=printf -w >/dev/null 2>&1 || { echo "BROKEN-C: $src"; brokenc=$((brokenc+1)); continue; }
    gcc -O3 -o "$o3" "$cfile" -Dlibc_printf=printf -w >/dev/null 2>&1 || { echo "BROKEN-C: $src"; brokenc=$((brokenc+1)); continue; }
    out0=$(timeout 5 "$o0" 2>&1); out3=$(timeout 5 "$o3" 2>&1)
    if [ "$out0" != "$out3" ]; then
        echo "MISCOMPILE (-O0 != -O3): $src"
        echo "   O0=[$out0]  O3=[$out3]"
        miscompiles=$((miscompiles+1))
    fi
done
echo "fuzz_annot: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$brokenc  MISCOMPILE=$miscompiles"
rm -rf "$SC"
[ $((brokenc + miscompiles)) -eq 0 ]
