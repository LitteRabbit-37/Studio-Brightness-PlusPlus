<#
.SYNOPSIS
  Builds the app (build.bat) with the Enterprise WDK instead of a Visual Studio install.

.DESCRIPTION
  The EWDK's SetupBuildEnv.cmd sets up MSVC but starts vsdevcmd with
  -winsdk=none, so INCLUDE and LIB lack the Windows SDK's user-mode folders
  (um, shared, ucrt, winrt) even though they are on the ISO. This adds them,
  then runs build.bat unchanged. Output: bin\studio-brightness-plusplus.exe.
#>
param(
  # The Enterprise WDK ISO (https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk);
  # default: the newest EWDK*.iso in Downloads.
  [string]$EwdkIso = (Get-ChildItem (Join-Path $env:USERPROFILE 'Downloads') -Filter 'EWDK*.iso' -EA SilentlyContinue |
                      Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $EwdkIso -or -not (Test-Path $EwdkIso)) { throw "EWDK ISO not found ('$EwdkIso') - pass -EwdkIso" }
$img = Get-DiskImage -ImagePath $EwdkIso
if (-not $img.Attached) {
  Write-Host 'mounting EWDK ISO'
  Mount-DiskImage -ImagePath $EwdkIso -StorageType ISO | Out-Null
  $img = Get-DiskImage -ImagePath $EwdkIso
}
$ewdk  = "$(($img | Get-Volume).DriveLetter):"
$setup = "$ewdk\BuildEnv\SetupBuildEnv.cmd"
$kits  = "$ewdk\Program Files\Windows Kits\10"
$ver   = (Get-ChildItem "$kits\Include" -Directory | Where-Object { $_.Name -match '^10\.' } |
          Sort-Object Name -Descending | Select-Object -First 1).Name

$inc = ('ucrt','um','shared','winrt','cppwinrt' | ForEach-Object { "$kits\Include\$ver\$_" }) -join ';'
$lib = ('ucrt','um' | ForEach-Object { "$kits\Lib\$ver\$_\x64" }) -join ';'

$bat = Join-Path $env:TEMP 'sbpp-build-ewdk.cmd'
Set-Content -Path $bat -Encoding ASCII -Value @(
  '@echo off',
  ('call "{0}" amd64 >nul' -f $setup),
  '@echo off',
  ('set "INCLUDE=%INCLUDE%;{0}"' -f $inc),
  ('set "LIB=%LIB%;{0}"' -f $lib),
  ('cd /d "{0}"' -f $repo),
  # By full path: the EWDK environment stops cmd searching the current directory.
  ('call "{0}\build.bat"' -f $repo),
  'exit /b %errorlevel%'
)
Write-Host ("EWDK {0}, SDK {1}" -f $ewdk, $ver)
& cmd.exe /c $bat
$rc = $LASTEXITCODE
[IO.File]::Delete($bat)
if ($rc -ne 0) { throw "build.bat failed (exit $rc)" }

$exe = Join-Path $repo 'bin\studio-brightness-plusplus.exe'
$v = (Get-Item $exe).VersionInfo
Write-Host ("built {0}  {1:N0} B  version {2}" -f $exe, (Get-Item $exe).Length, $v.FileVersion)
