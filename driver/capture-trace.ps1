#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Records what Windows' brightness stack and SbppMonFilter do, as an ETW trace.
  Run it elevated; it changes nothing permanent.

.DESCRIPTION
  ETW providers (all self-describing, so no symbols are needed to read them):
    StudioBrightnessPlusPlus.SbppMonFilter {c43e7bfe-...}  this driver: attach,
                                           app connect, every _BCL/_BQC/_BCM
                                           answered, every report from the app
    Microsoft.Windows.Graphics.Display     {deb96c0a-...}  monitor.sys:
                                           MonitorInitBrightness, SetBrightnessFailure, ...
    Microsoft-Windows-UserModePowerService                 user-mode power service
    Microsoft-Windows-Kernel-Power                         power manager
    Microsoft-Windows-Display                              display stack

  By default it records for -Seconds while you use the brightness controls.
  With -RestartDevice it first restarts the monitor devnode, so monitor.sys's
  brightness initialisation is in the trace too (the screen may flicker once).
  The .etl and a decoded .xml land in driver\out\trace\.
#>
param(
  [ValidateRange(3, 600)][int]$Seconds = 15,
  [switch]$RestartDevice
)
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dir  = Join-Path $here 'out\trace'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$etl = Join-Path $dir "brightness-$stamp.etl"
$xml = Join-Path $dir "brightness-$stamp.xml"
$pf  = Join-Path $dir 'providers.txt'
$session = 'SbppBrightnessTrace'

Set-Content -Path $pf -Encoding ASCII -Value @(
  '{c43e7bfe-0d43-589f-3d47-c8b80e7a7919} 0xFFFFFFFFFFFFFFFF 0xFF',
  '{deb96c0a-d2d9-5868-a5d5-50ee13513c8b} 0xFFFFFFFFFFFFFFFF 0xFF',
  '{CE8DEE0B-D539-4000-B0F8-77BED049C590} 0xFFFFFFFFFFFFFFFF 0xFF',
  '{331C3B3A-2005-44C2-AC5E-77220C37D6B4} 0xFFFFFFFFFFFFFFFF 0xFF',
  '{6ECE3302-FEE1-4EA9-8B88-086D459ED976} 0xFFFFFFFFFFFFFFFF 0xFF'
)

& logman.exe stop $session -ets 2>$null | Out-Null   # a leftover session from an aborted run
Write-Host "starting trace -> $etl"
& logman.exe start $session -pf $pf -o $etl -ets -bs 256 -nb 64 256 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "logman start failed ($LASTEXITCODE)" }

try {
  if ($RestartDevice) {
    $mon = Get-PnpDevice -Class Monitor -PresentOnly | Where-Object { $_.InstanceId -like 'DISPLAY\APPAE3A\*' } | Select-Object -First 1
    if (-not $mon) { throw 'Studio Display monitor devnode (DISPLAY\APPAE3A) not found' }
    Write-Host 'restarting the monitor devnode (monitor.sys re-initialises brightness)'
    & pnputil.exe /restart-device $mon.InstanceId | Out-Null
  }
  Write-Host "recording for $Seconds s - use the Settings / Quick Settings slider, brightness keys, the tray slider"
  Start-Sleep -Seconds $Seconds
} finally {
  & logman.exe stop $session -ets | Out-Null
}

Write-Host 'decoding'
& tracerpt.exe $etl -o $xml -of XML -y | Out-Null

[xml]$doc = Get-Content -Raw $xml
$events = @($doc.Events.Event)
Write-Host ("{0} events" -f $events.Count)
$events | Group-Object { $_.System.Provider.Name } | Sort-Object Count -Descending |
  ForEach-Object { Write-Host ("  {0,5}  {1}" -f $_.Count, $_.Name) }
Write-Host ''
Write-Host "decoded: $xml"
