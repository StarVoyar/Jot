#!/bin/sh
# Jot compiler test runner (Linux/macOS).
# Builds jotc, then checks every examples/**/*.jot and tests/stress/*.jot
# produces byte-identical output at -O0, -O1 and -O2, plus a generated
# ~80k-token file (token-buffer growth), an unterminated-string expect-fail
# check, and CLI smoke tests. No checked-in baselines: the three
# optimization levels must agree.
# Usage: sh tests/run_unix.sh  (run from the repo root)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
make build link >/dev/null 2>&1 || { echo "FAIL build"; exit 1; }
JOTC="build/bin/jotc"
TMP="${TMPDIR:-/tmp}/jot_test_unix"
mkdir -p "$TMP"
PASS=0; FAIL=0
# Crash-regression: ~80k tokens forces token-buffer growth past the old
# 65536 cap. Generated here (not committed) to keep the repo lean.
{
i=1; while [ "$i" -le 1500 ]; do
  printf 'fn public g%d(num a) -> num {\n  num t = self.a;\n' "$i"
  j=1; while [ "$j" -le 8 ]; do printf '  t += %d;\n' "$j"; j=$((j+1)); done
  printf '  return(t);\n}\n'
  i=$((i+1))
done
printf 'fn public main() {\n  num t = 0;\n'
i=1; while [ "$i" -le 1500 ]; do printf '  t += g%d(1);\n' "$i"; i=$((i+1)); done
printf '  print(t);\n  print("\\n");\n  return(0);\n}\nmain();\n'
} > "$TMP/bigtokens.jot"
for f in examples/*/*.jot tests/stress/*.jot "$TMP/bigtokens.jot"; do
  d=$(dirname "$f"); b=$(basename "$f" .jot)
  case "$f" in /*) ff="$f"; dd="$d";; *) ff="$ROOT/$f"; dd="$ROOT/$d";; esac
  tag="$d/$b"
  ok=1; o0=""; o1=""; o2=""; c0=""; c1=""; c2=""
  n=0
  for opt in -O0 -O1 -O2; do
    n=$((n+1))
    asm="$TMP/t_${b}_${opt}.asm"; obj="$TMP/t_${b}_${opt}.o"; exe="$TMP/t_${b}_${opt}"
    (cd "$dd" && "$ROOT/$JOTC" "$ff" "$asm" "$opt") >/dev/null 2>&1 || ok=0
    nasm -f elf64 "$asm" -o "$obj" >/dev/null 2>&1 || ok=0
    gcc "$obj" -o "$exe" >/dev/null 2>&1 || ok=0
    out=$(printf '42\nhello\n' | "$exe" 2>/dev/null); code=$?
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
# Crash-regression: an unterminated string must error (exit 1), never crash.
printf 'fn public main() { print("abc); return(0); } main();' > "$TMP/unterm.jot"
"$JOTC" "$TMP/unterm.jot" "$TMP/unterm.asm" -O1 >/dev/null 2>&1
if [ "$?" -eq 1 ]; then PASS=$((PASS+1)); else echo "FAIL unterm"; FAIL=$((FAIL+1)); fi
echo "done pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
