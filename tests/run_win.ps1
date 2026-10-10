# Jot compiler test runner (Windows).
# Builds jotc, then checks every examples/**/*.jot and tests/stress/*.jot
# produces byte-identical output at -O0, -O1 and -O2, plus a generated
# ~80k-token file (token-buffer growth), an unterminated-string expect-fail
# check, and CLI smoke tests. No checked-in baselines: the three
# optimization levels must agree.
# Usage: powershell -File tests/run_win.ps1  (run from the repo root)
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if (-not $root) { $root = "." }
Set-Location $root

& make build link >$null 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "FAIL build"; exit 1 }

$jotc = Join-Path $root "build/bin/jotc.exe"
$tmp = Join-Path $env:TEMP "jot_test_win"
New-Item -ItemType Directory -Path $tmp -Force >$null
$pass = 0; $fail = 0

$files = @()
$files += Get-ChildItem -Recurse (Join-Path $root "examples") -Filter *.jot
$files += Get-ChildItem (Join-Path $root "tests/stress") -Filter *.jot
# Crash-regression: ~80k tokens forces token-buffer growth past the old
# 65536 cap. Generated here (not committed) to keep the repo lean.
$bt = Join-Path $tmp "bigtokens.jot"
$sb = New-Object System.Text.StringBuilder
1..1500 | ForEach-Object {
  [void]$sb.Append("fn public g$_" + "(num a) -> num {`n  num t = self.a;`n")
  1..8 | ForEach-Object { [void]$sb.Append("  t += $_;`n") }
  [void]$sb.Append("  return(t);`n}`n")
}
[void]$sb.Append("fn public main() {`n  num t = 0;`n")
1..1500 | ForEach-Object { [void]$sb.Append("  t += g$_(1);`n") }
[void]$sb.Append("  print(t);`n  print(`"\n`");`n  return(0);`n}`nmain();`n")
[System.IO.File]::WriteAllText($bt, $sb.ToString())
$files += Get-Item $bt
# Crash-regression: an unterminated string must error (exit 1), never crash.
$unterm = Join-Path $tmp "unterm.jot"
Set-Content -Path $unterm -Value 'fn public main() { print("abc); return(0); } main();' -NoNewline -Encoding ASCII
& $jotc $unterm (Join-Path $tmp "unterm.asm") -O1 >$null 2>$null
if ($LASTEXITCODE -eq 1) { $pass++ } else { Write-Output "FAIL unterm exit=$LASTEXITCODE"; $fail++ }
foreach ($f in $files) {
  $tag = $f.Directory.Name + "/" + $f.BaseName
  $outs = @()
  $codes = @()
  $ok = $true
  foreach ($opt in @("-O0", "-O1", "-O2")) {
    $asm = Join-Path $tmp ("t_" + $f.BaseName + "_" + $opt + ".asm")
    $obj = Join-Path $tmp ("t_" + $f.BaseName + "_" + $opt + ".obj")
    $exe = Join-Path $tmp ("t_" + $f.BaseName + "_" + $opt + ".exe")
    Push-Location $f.DirectoryName
    & $jotc $f.FullName $asm $opt >$null 2>$null
    $cc = $LASTEXITCODE
    if ($cc -eq 0) { & nasm -f win64 $asm -o $obj >$null 2>&1; $cc = $LASTEXITCODE }
    if ($cc -eq 0) { & gcc $obj -o $exe >$null 2>&1; $cc = $LASTEXITCODE }
    if ($cc -eq 0) {
      $o = "42`nhello`n" | & $exe 2>$null | Out-String
      $codes += $LASTEXITCODE
      $outs += $(if ($o -ne $null) { $o -replace "`r`n", "`n" } else { "" })
    } else {
      $ok = $false
    }
    Pop-Location
    if (-not $ok) { break }
  }
  if ($ok -and $outs[0] -ceq $outs[1] -and $outs[0] -ceq $outs[2] -and $codes[0] -eq $codes[1] -and $codes[0] -eq $codes[2]) {
    $pass++
  } else {
    Write-Output "FAIL $tag"
    $fail++
  }
}

# CRLF sources must behave like LF sources.
$crlfSrc = (Get-Content (Join-Path $root "examples/opt/fold.jot") -Raw) -replace "`n", "`r`n"
$crlfJot = Join-Path $tmp "crlf.jot"
Set-Content -Path $crlfJot -Value $crlfSrc -NoNewline -Encoding ASCII
& $jotc $crlfJot (Join-Path $tmp "crlf.asm") -O1 >$null 2>$null
if ($LASTEXITCODE -eq 0) { $pass++ } else { Write-Output "FAIL crlf"; $fail++ }

# CLI smoke tests.
& $jotc --help >$null 2>$null; if ($LASTEXITCODE -eq 0) { $pass++ } else { Write-Output "FAIL help-exit"; $fail++ }
& $jotc (Join-Path $root "examples/opt/fold.jot") --emit-ir -O1 >$null 2>$null; if ($LASTEXITCODE -eq 0) { $pass++ } else { Write-Output "FAIL emit-ir"; $fail++ }
& $jotc (Join-Path $root "examples/opt/fold.jot") --emit-asm -O1 >$null 2>$null; if ($LASTEXITCODE -eq 0) { $pass++ } else { Write-Output "FAIL emit-asm"; $fail++ }
& $jotc (Join-Path $root "examples/opt/fold.jot") (Join-Path $tmp "x.asm") --target bogus >$null 2>$null; if ($LASTEXITCODE -ne 0) { $pass++ } else { Write-Output "FAIL bad-target"; $fail++ }
& $jotc (Join-Path $root "does-not-exist.jot") (Join-Path $tmp "x.asm") >$null 2>$null; if ($LASTEXITCODE -ne 0) { $pass++ } else { Write-Output "FAIL missing-file"; $fail++ }

Write-Output "done pass=$pass fail=$fail"
if ($fail -ne 0) { exit 1 }
