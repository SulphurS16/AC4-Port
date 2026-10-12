# Downloads and unpacks the pinned, project-local toolchain into local\tools (no admin rights,
# nothing installed system-wide). Archives are verified against pinned SHA-256 values.
#   .\tools\bootstrap.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dl = Join-Path $root "local\downloads"
$tools = Join-Path $root "local\tools"
New-Item -ItemType Directory -Force -Path $dl, $tools | Out-Null

$items = @(
  @{ url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/llvm-mingw-20260908-ucrt-x86_64.zip";
     sha = "1bcf74d06b724aeecaa6412ca85f5b26fb1da770e7cdcefa9263c9c5c3ad34b6"; dest = ""; check = "llvm-mingw-20260908-ucrt-x86_64" },
  @{ url = "https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip";
     sha = "4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab"; dest = ""; check = "cmake-4.4.3-windows-x86_64" },
  @{ url = "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip";
     sha = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"; dest = "ninja"; check = "ninja" },
  @{ url = "https://www.python.org/ftp/python/3.13.9/python-3.13.9-embed-amd64.zip";
     sha = "91d828c2da3a029b41699e918674a0cb379c02cf20dab9c501306885f837402a"; dest = "python"; check = "python" }
)
foreach ($it in $items) {
  $file = Join-Path $dl (Split-Path -Leaf $it.url)
  if (-not (Test-Path $file)) { Write-Host "downloading $($it.url)"; Invoke-WebRequest -Uri $it.url -OutFile $file -UseBasicParsing }
  $h = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
  if ($h -ne $it.sha) { throw "hash mismatch for $file ($h)" }
  if (Test-Path (Join-Path $tools $it.check)) { Write-Host "ok $($it.check)"; continue }
  $target = if ($it.dest) { Join-Path $tools $it.dest } else { $tools }
  Expand-Archive -Path $file -DestinationPath $target -Force
  Write-Host "unpacked $($it.check)"
}
