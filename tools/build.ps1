# Configure (first time) and build with the project-local toolchain.
#   .\tools\build.ps1 [-Config Release|RelWithDebInfo|Debug] [-Target <name>] [-Clean]
param(
  [string]$Config = "Release",
  [string]$Target = "",
  [switch]$Clean
)
# Windows PowerShell 5.1 turns a native tool's stderr lines into error records; under a caller's
# "Stop" preference CMake's "GLOB mismatch" notice would abort the build. Exit codes decide here.
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "env.ps1")
$root = Split-Path -Parent $PSScriptRoot
$bdir = Join-Path $root ("local\build\" + $Config.ToLower())
if ($Clean -and (Test-Path $bdir)) { Remove-Item -Recurse -Force $bdir }
if (-not (Test-Path (Join-Path $bdir "build.ninja"))) {
  cmake -S $root -B $bdir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($Target) { cmake --build $bdir --target $Target } else { cmake --build $bdir }
exit $LASTEXITCODE
