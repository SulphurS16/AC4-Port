# Downloads and unpacks the pinned, project-local toolchain into local\tools (no admin rights,
# nothing installed system-wide). The list, with SHA-256 values, is data\toolchain.txt (the
# launcher reads the same file).
#   .\tools\bootstrap.ps1 [-BuildOnly]   (-BuildOnly: skip development-only tools)
param([switch]$BuildOnly)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dl = Join-Path $root "local\downloads"
$tools = Join-Path $root "local\tools"
New-Item -ItemType Directory -Force -Path $dl, $tools | Out-Null

$items = Get-Content (Join-Path $root "data\toolchain.txt") | Where-Object { $_ -and -not $_.StartsWith("#") } | ForEach-Object {
  $f = $_ -split "\s+"
  @{ name = $f[0]; sha = $f[1]; dest = $f[2]; check = $f[3]; use = $f[5]; url = $f[6] }
}
foreach ($it in $items) {
  if ($BuildOnly -and $it.use -ne "build") { continue }
  $file = Join-Path $dl (Split-Path -Leaf $it.url)
  if (-not (Test-Path $file)) { Write-Host "downloading $($it.url)"; Invoke-WebRequest -Uri $it.url -OutFile $file -UseBasicParsing }
  $h = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
  if ($h -ne $it.sha) { throw "hash mismatch for $file ($h)" }
  if (Test-Path (Join-Path $tools $it.check)) { Write-Host "ok $($it.check)"; continue }
  $target = if ($it.dest -ne ".") { Join-Path $tools $it.dest } else { $tools }
  New-Item -ItemType Directory -Force -Path $target | Out-Null
  # Windows' bsdtar unpacks zips far faster than Expand-Archive; fall back if it is missing.
  $tar = Join-Path $env:SystemRoot "System32\tar.exe"
  if (Test-Path $tar) {
    & $tar -xf $file -C $target
    if ($LASTEXITCODE -ne 0) { throw "tar failed for $file" }
  } else {
    Expand-Archive -Path $file -DestinationPath $target -Force
  }
  Write-Host "unpacked $($it.check)"
}
