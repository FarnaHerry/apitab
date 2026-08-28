# ci-package.ps1 - assemble a Windows portable package from built artifacts.
# Keep ASCII-only for Windows PowerShell compatibility.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = $args[0]
$k6 = $args[1]
if (-not $exe -or -not $k6) { throw "usage: ci-package.ps1 <apitab.exe> <k6.exe>" }
if (-not (Test-Path $exe -PathType Leaf)) { throw "executable not found: $exe" }
if (-not (Test-Path $k6 -PathType Leaf)) { throw "k6 not found: $k6" }
$versionLine = Get-Content (Join-Path $root "CMakeLists.txt") | Where-Object { $_ -match '^\s*project\(apitab\s+VERSION\s+' } | Select-Object -First 1
if (-not $versionLine) { throw "cannot parse version from CMakeLists.txt" }
$version = $versionLine -replace '^\s*project\(apitab\s+VERSION\s+([0-9.]+).*', '$1'
$dist = Join-Path $root "dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $dist "engines") | Out-Null
Copy-Item $exe (Join-Path $dist "apitab.exe")
Copy-Item (Join-Path $root "assets") (Join-Path $dist "assets") -Recurse
Copy-Item $k6 (Join-Path $dist "engines\k6.exe")
& (Join-Path $dist "engines\k6.exe") version
if ($LASTEXITCODE -ne 0) { throw "k6 version check failed" }
$zip = Join-Path $root "apitab-v$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip -CompressionLevel Optimal
Write-Host "produced: $zip"
