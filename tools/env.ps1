# Dot-source to put the project-local toolchain first on PATH for this PowerShell session:
#   . .\tools\env.ps1
# Toolchain lives in local\tools (data\toolchain.txt lists the pinned versions and hashes).
$root = Split-Path -Parent $PSScriptRoot
$t = Join-Path $root "local\tools"
$dirs = Get-Content (Join-Path $root "data\toolchain.txt") | Where-Object { $_ -and -not $_.StartsWith("#") } | ForEach-Object {
  Join-Path $t (($_ -split "\s+")[4])
}
foreach ($d in $dirs) { if (-not (Test-Path $d)) { Write-Warning "missing toolchain dir $d (run tools\bootstrap.ps1)" } }
$env:PATH = ($dirs -join ';') + ';' + $env:PATH
$env:AC4_ROOT = $root
