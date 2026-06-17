#!/bin/bash
# Drop-in for the test runner's $GCC: compile the generated program with tcc
# (header swapped to strada_runtime_tcc.h), then link the object with gcc
# (tcc can't relocate the gcc-built runtime). Mirrors `strada --tcc`.
prog=""; out=""; pass=(); incs=()
args=("$@"); n=${#args[@]}; i=0
while [ "$i" -lt "$n" ]; do
  a="${args[$i]}"
  if [ "$a" = "-o" ]; then out="${args[$((i+1))]}"; i=$((i+2)); continue; fi
  case "$a" in
    -I*) incs+=("$a"); pass+=("$a") ;;
    *.c) if [ -z "$prog" ] && [[ "$a" != *strada_runtime* ]]; then prog="$a"; else pass+=("$a"); fi ;;
    *)   pass+=("$a") ;;
  esac
  i=$((i+1))
done
if [ -z "$prog" ]; then exec gcc "$@"; fi
swapped="${prog%.c}.tccin.c"
sed 's/strada_runtime\.h/strada_runtime_tcc.h/' "$prog" > "$swapped"
obj="${prog%.c}.tcc.o"
if ! tcc -c "$swapped" "${incs[@]}" -o "$obj" 2>&1; then
  exit 2
fi
# tcc drops __attribute__((weak)); re-weaken those symbols so optional
# import_object OOP-init hooks resolve to NULL instead of undefined.
if command -v objcopy >/dev/null 2>&1; then
  for ws in $(grep '__attribute__((weak))' "$swapped" 2>/dev/null | grep -oE '[A-Za-z_][A-Za-z0-9_]*\(' | tr -d '(' | grep -v '^__attribute__$' | sort -u); do
    objcopy --weaken-symbol="$ws" "$obj" 2>/dev/null || true
  done
fi
exec gcc -Wl,-z,noexecstack -o "$out" "$obj" "${pass[@]}"
