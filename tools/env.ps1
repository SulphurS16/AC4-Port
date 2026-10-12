# Dot-source to put the project-local toolchain first on PATH for this PowerShell session:
#   . .\tools\env.ps1
# Toolchain lives in local\tools (see tools\bootstrap.ps1 for pinned versions/hashes).
$root = Split-Path -Parent $PSScriptRoot
$t = Join-Path $root "local\tools"
$dirs = @(
  (Join-Path $t "llvm-mingw-20260908-ucrt-x86_64\bin"),
  (Join-Path $t "cmake-4.4.3-windows-x86_64\bin"),
  (Join-Path $t "ninja"),
  (Join-Path $t "python")
)
foreach ($d in $dirs) { if (-not (Test-Path $d)) { Write-Warning "missing toolchain dir $d (run tools\bootstrap.ps1)" } }
$env:PATH = ($dirs -join ';') + ';' + $env:PATH
$env:AC4_ROOT = $root
