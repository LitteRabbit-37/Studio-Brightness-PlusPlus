<#
.SYNOPSIS
  Builds and test-signs the SbppMonFilter driver with the Enterprise WDK.

.DESCRIPTION
  Runs unelevated. Mounts the EWDK ISO if needed, builds the KMDF project,
  then signs the .sys, generates the catalog and signs that.

  Output, all under driver\out\ (git-ignored):
    SbppMonFilter\           the driver package: .sys, .inf, .cat
    SbppMonFilter-test.cer   public half of the test certificate, which
                             install-dev.ps1 imports into the machine stores
    msbuild.log              full build log

  The test certificate is created once in Cert:\CurrentUser\My with a
  non-exportable private key. It never leaves this user's store.
#>
param(
  [ValidateSet('Release','Debug')]
  [string]$Configuration = 'Release',
  # The Enterprise WDK ISO (https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk);
  # default: the newest EWDK*.iso in Downloads.
  [string]$EwdkIso = (Get-ChildItem (Join-Path $env:USERPROFILE 'Downloads') -Filter 'EWDK*.iso' -EA SilentlyContinue |
                      Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
)
$ErrorActionPreference = 'Stop'

$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$projDir = Join-Path $here 'SbppMonFilter'
$proj    = Join-Path $projDir 'SbppMonFilter.vcxproj'
$out     = Join-Path $here 'out'
$pkg     = Join-Path $out 'SbppMonFilter'
$log     = Join-Path $out 'msbuild.log'
$cerPath = Join-Path $out 'SbppMonFilter-test.cer'
$certSubject = 'CN=SbppMonFilter Test Signing'

New-Item -ItemType Directory -Force -Path $out | Out-Null

# --- 1. EWDK ---------------------------------------------------------------
if (-not $EwdkIso -or -not (Test-Path $EwdkIso)) { throw "EWDK ISO not found ('$EwdkIso') - pass -EwdkIso" }
$img = Get-DiskImage -ImagePath $EwdkIso
if (-not $img.Attached) {
  Write-Host "mounting EWDK ISO"
  Mount-DiskImage -ImagePath $EwdkIso -StorageType ISO | Out-Null
  $img = Get-DiskImage -ImagePath $EwdkIso
}
$drive = ($img | Get-Volume).DriveLetter
$ewdk  = "${drive}:"
$setup = "$ewdk\BuildEnv\SetupBuildEnv.cmd"
if (-not (Test-Path $setup)) { throw "SetupBuildEnv.cmd not found at $setup" }

$kitBin   = Get-ChildItem "$ewdk\Program Files\Windows Kits\10\bin" -Directory |
            Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1
$signtool = Join-Path $kitBin.FullName 'x64\signtool.exe'
$inf2cat  = Join-Path $kitBin.FullName 'x86\Inf2Cat.exe'
foreach ($t in $signtool, $inf2cat) { if (-not (Test-Path $t)) { throw "missing tool: $t" } }
Write-Host ("EWDK at {0}  kit {1}" -f $ewdk, $kitBin.Name)

# --- 2. Build ----------------------------------------------------------------
# A temporary .cmd avoids every PowerShell -> cmd quoting pitfall.
$bat = Join-Path $out 'build.cmd'
$lines = @(
  '@echo off',
  ('call "{0}" amd64 >nul' -f $setup),
  '@echo off',
  'if errorlevel 1 exit /b 1',
  ('msbuild "{0}" /nologo /t:Rebuild /p:Configuration={1} /p:Platform=x64 /v:minimal /fl "/flp:logfile={2};verbosity=normal"' -f $proj, $Configuration, $log),
  'exit /b %errorlevel%'
)
Set-Content -Path $bat -Value $lines -Encoding ASCII

Write-Host "building $Configuration|x64"
& cmd.exe /c $bat
if ($LASTEXITCODE -ne 0) { throw "msbuild failed (exit $LASTEXITCODE) - see $log" }

# The stamped INF is the one whose $KMDFVERSION$ placeholder was replaced.
$buildDir = Join-Path $projDir "x64\$Configuration"
$sys = Get-ChildItem $buildDir -Recurse -Filter 'SbppMonFilter.sys' | Select-Object -First 1
$inf = Get-ChildItem $buildDir -Recurse -Filter 'SbppMonFilter.inf' |
       Where-Object { (Get-Content $_.FullName -Raw) -notmatch '\$KMDFVERSION\$' } | Select-Object -First 1
if (-not $sys) { throw "SbppMonFilter.sys not found under $buildDir" }
if (-not $inf) { throw "stamped SbppMonFilter.inf not found under $buildDir" }

if (Test-Path $pkg) { Get-ChildItem $pkg -File | ForEach-Object { [IO.File]::Delete($_.FullName) } }
New-Item -ItemType Directory -Force -Path $pkg | Out-Null
Copy-Item $sys.FullName (Join-Path $pkg 'SbppMonFilter.sys')
Copy-Item $inf.FullName (Join-Path $pkg 'SbppMonFilter.inf')

# --- 3. Test certificate -----------------------------------------------------
$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $certSubject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(7) } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
  Write-Host "creating test certificate '$certSubject' in Cert:\CurrentUser\My"
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certSubject `
            -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 3072 `
            -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(2)
}
Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null

# --- 4. Sign .sys, build the catalog, sign the catalog ----------------------
$sysOut = Join-Path $pkg 'SbppMonFilter.sys'
& $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My $sysOut
if ($LASTEXITCODE -ne 0) { throw "signtool failed on the .sys" }

& $inf2cat /driver:$pkg /os:10_X64 /uselocaltime | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed" }

$catOut = Join-Path $pkg 'SbppMonFilter.cat'
& $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My $catOut
if ($LASTEXITCODE -ne 0) { throw "signtool failed on the .cat" }

# --- 5. Summary ----------------------------------------------------------------
Write-Host ""
Write-Host "package: $pkg"
Get-ChildItem $pkg -File | ForEach-Object {
  "  {0,-20} {1,8:N0} B  sha256 {2}" -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash
}
Write-Host ("test cert: {0}  thumbprint {1}  expires {2:yyyy-MM-dd}" -f $cert.Subject, $cert.Thumbprint, $cert.NotAfter)
Write-Host "cert file: $cerPath"
Write-Host "BUILD OK"
