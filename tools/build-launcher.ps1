# Builds AC4-Launcher.exe from this repository's sources with the pinned toolchain (downloaded
# into local\tools, checked against data\toolchain.txt) and puts it in the repository folder,
# where it uses this folder's sources. The release page's launcher is built the same way from the
# same sources (reproducibly: same tools, same file) and also carries those sources inside it
# (CMake option AC4_SOURCE_ZIP), so it needs no other files.
#   powershell -ExecutionPolicy Bypass -File tools\build-launcher.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "bootstrap.ps1") -BuildOnly
& (Join-Path $PSScriptRoot "build.ps1") -Target ac4-launcher
if ($LASTEXITCODE -ne 0) { throw "launcher build failed" }
Copy-Item -Force (Join-Path $root "local\build\release\AC4-Launcher.exe") (Join-Path $root "AC4-Launcher.exe")
$h = (Get-FileHash -Algorithm SHA256 (Join-Path $root "AC4-Launcher.exe")).Hash.ToLower()
Write-Output "AC4-Launcher.exe built (sha256 $h)"
