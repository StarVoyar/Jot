#!/bin/sh
# Jot compiler test runner (Linux/macOS).
# Builds jotc, then checks every examples/**/*.jot produces byte-identical
# output at -O0, -O1 and -O2, plus CLI smoke tests. No checked-in baselines:
# the three optimization levels must agree with each other.
# Usage: sh tests/run_unix.sh  (run from the repo root)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
make build link >/dev/null 2>&1 || { echo "FAIL build"; exit 1; }
JOTC="build/bin/jotc"
TMP="${TMPDIR:-/tmp}/jot_test_unix"
mkdir -p "$TMP"
PASS=0; FAIL=0
for f in examples/*/*.jot; do
  d=$(dirname "$f"); b=$(basename "$f" .jot)
  tag="$d/$b"
  ok=1; o0=""; o1=""; o2=""; c0=""; c1=""; c2=""
  n=0
  for opt in -O0 -O1 -O2; do
    n=$((n+1))
    asm="$TMP/t_${b}_${opt}.asm"; obj="$TMP/t_${b}_${opt}.o"; exe="$TMP/t_${b}_${opt}"
    (cd "$ROOT/$d" && "$ROOT/$JOTC" "$ROOT/$f" "$ROOT/$asm" "$opt") >/dev/null 2>&1 || ok=0
    nasm -f elf64 "$ROOT/$asm" -o "$ROOT/$obj" >/dev/null 2>&1 || ok=0
    gcc "$ROOT/$obj" -o "$ROOT/$exe" >/dev/null 2>&1 || ok=0
    out=$(printf '42\nhello\n' | "$ROOT/$exe" 2>/dev/null); code=$?
    if [ "$n" -eq 1 ]; then o0="$out"; c0="$code"; fi
    if [ "$n" -eq 2 ]; then o1="$out"; c1="$code"; fi
    if [ "$n" -eq 3 ]; then o2="$out"; c2="$code"; fi
  done
  if [ "$ok" -eq 1 ] && [ "$o0" = "$o1" ] && [ "$o0" = "$o2" ] && [ "$c0" = "$c1" ] && [ "$c0" = "$c2" ]; then
    PASS=$((PASS+1))
  else
    echo "FAIL $tag"; FAIL=$((FAIL+1))
  fi
done
# CRLF sources must behave like LF sources.
python3 -c "import sys; d=open('examples/opt/fold.jot','rb').read().replace(b'\r\n',b'\n').replace(b'\n',b'\r\n'); open('$TMP/crlf.jot','wb').write(d)" 2>/dev/null || \
  sed 's/$/\r/' examples/opt/fold.jot > "$TMP/crlf.jot"
if "$JOTC" "$TMP/crlf.jot" "$TMP/crlf.asm" -O1 >/dev/null 2>&1; then PASS=$((PASS+1)); else echo "FAIL crlf"; FAIL=$((FAIL+1)); fi
# CLI smoke tests.
if "$JOTC" --help >/dev/null 2>&1; then PASS=$((PASS+1)); else echo "FAIL help-exit"; FAIL=$((FAIL+1)); fi
if "$JOTC" examples/opt/fold.jot --emit-ir -O1 >/dev/null 2>&1; then PASS=$((PASS+1)); else echo "FAIL emit-ir"; FAIL=$((FAIL+1)); fi
if "$JOTC" examples/opt/fold.jot --emit-asm -O1 >/dev/null 2>&1; then PASS=$((PASS+1)); else echo "FAIL emit-asm"; FAIL=$((FAIL+1)); fi
if "$JOTC" examples/opt/fold.jot "$TMP/x.asm" --target bogus >/dev/null 2>&1; then echo "FAIL bad-target"; FAIL=$((FAIL+1)); else PASS=$((PASS+1)); fi
if "$JOTC" does-not-exist.jot "$TMP/x.asm" >/dev/null 2>&1; then echo "FAIL missing-file"; FAIL=$((FAIL+1)); else PASS=$((PASS+1)); fi
echo "done pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
