# Builds the per-machine MSI from the already-built exe and the generated version.h.
# Run build.bat first (it produces bin\studio-brightness-plusplus.exe and include\version.h).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vh = Join-Path $root 'include\version.h'
if (-not (Test-Path $vh)) { throw "include\version.h missing - run build.bat first" }
$txt = Get-Content $vh -Raw
$maj  = [regex]::Match($txt, 'SBPP_VER_MAJOR (\d+)').Groups[1].Value
$min  = [regex]::Match($txt, 'SBPP_VER_MINOR (\d+)').Groups[1].Value
$pat  = [regex]::Match($txt, 'SBPP_VER_PATCH (\d+)').Groups[1].Value
$full = [regex]::Match($txt, 'SBPP_VERSION_NARROW "([^"]+)"').Groups[1].Value
# MSI ProductVersion is numeric only (no -beta suffix); the 3 fields drive upgrade comparison.
$prodVer = "$maj.$min.$pat"

$exe = Join-Path $root 'bin\studio-brightness-plusplus.exe'
if (-not (Test-Path $exe)) { throw "bin\studio-brightness-plusplus.exe missing - run build.bat first" }

$wxs = Join-Path $root 'installer\studio-brightness-plusplus.wxs'
$msi = Join-Path $root ("bin\studio-brightness-plusplus-$full.msi")

$lic = Join-Path $root 'installer\license.rtf'

$wix = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
if (-not (Test-Path $wix)) { $wix = 'wix' }

# Ensure the matching (5.0.2) UI extension is present, then build with the WixUI dialogs.
& $wix extension add -g WixToolset.UI.wixext/5.0.2 2>&1 | Out-Null
& $wix build $wxs -arch x64 -ext WixToolset.UI.wixext -d "ProductVersion=$prodVer" -d "ExeSource=$exe" -d "LicenseRtf=$lic" -o $msi
if ($LASTEXITCODE -ne 0) { throw "wix build failed ($LASTEXITCODE)" }
Write-Host "MSI -> $msi  (ProductVersion $prodVer, full $full)"
