<#
.SYNOPSIS
  Shows the state of SbppMonFilter and Windows' view of the display. No elevation needed.

.DESCRIPTION
  - the Studio Display monitor devnode: is SbppMonFilter in its driver stack,
    below monitor.sys, and is BrightnessControl set
  - the filter's status snapshot: setup results, ACPI notification
    registration, whether the app is connected
  - Windows' brightness for the display (root\wmi WmiMonitorBrightness), which
    monitor.sys publishes once it has taken the display as its brightness target

  For a step-by-step record of what Windows asks the driver, use
  capture-trace.ps1.

.EXAMPLE
  .\driver-status.ps1
.EXAMPLE
  .\driver-status.ps1 -SetTo 60     # set 60 % through Windows (WmiSetBrightness)
#>
param(
  [ValidateRange(-1, 100)][int]$SetTo = -1
)
$ErrorActionPreference = 'Stop'

function Section([string]$t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Hex($v) { if ($null -eq $v) { '-' } else { '0x{0:X8}' -f [uint32]$v } }
function Result($v) { if ($null -eq $v) { '-' } elseif ([uint32]$v -eq 0) { 'ok' } else { Hex $v } }
function Get-StudioBrightness {
  Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightness -EA SilentlyContinue |
    Where-Object { $_.InstanceName -like 'DISPLAY\APPAE3A*' } | Select-Object -First 1
}

# --- Monitor devnode ------------------------------------------------------------
Section 'Studio Display monitor devnode'
$mon = Get-PnpDevice -Class Monitor -PresentOnly | Where-Object { $_.InstanceId -like 'DISPLAY\APPAE3A\*' } | Select-Object -First 1
if (-not $mon) {
  Write-Host '  DISPLAY\APPAE3A not present - is the Studio Display connected?' -ForegroundColor Yellow
} else {
  $prop  = { param($k) (Get-PnpDeviceProperty -InstanceId $mon.InstanceId -KeyName $k -EA SilentlyContinue).Data }
  $stack = @(& $prop 'DEVPKEY_Device_Stack')
  "  status  {0}, problem code {1}" -f $mon.Status, (& $prop 'DEVPKEY_Device_ProblemCode')
  "  stack   {0}" -f ($stack -join ' -> ')
  $iF = [array]::IndexOf($stack, '\Driver\SbppMonFilter'); $iM = [array]::IndexOf($stack, '\Driver\monitor')
  "  filter  {0}" -f $(if ($iF -lt 0) { 'not in the stack' } elseif ($iF -gt $iM) { 'below monitor.sys, as intended' } else { 'ABOVE monitor.sys - unexpected' })
  $drv = & $prop 'DEVPKEY_Device_Driver'
  $bc  = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drv" -Name BrightnessControl -EA SilentlyContinue).BrightnessControl
  "  BrightnessControl {0}" -f $(if ($null -eq $bc) { 'absent' } else { $bc })
}

# --- Filter ---------------------------------------------------------------------
Section 'Filter'
$p = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\SbppMonFilter\Parameters' -EA SilentlyContinue
if (-not $p -or -not $p.AttachTime) {
  Write-Host '  nothing recorded - is the package installed (install-dev.ps1)?' -ForegroundColor Yellow
} else {
  "  attached  {0}" -f [DateTime]::FromFileTimeUtc([long]$p.AttachTime).ToLocalTime()
  "  setup     I/O queue {0}, ACPI interface {1}, bridge device {2}" -f (Result $p.QueueStatus), (Result $p.AcpiInterfaceStatus), (Result $p.ControlStatus)
  "  monitor.sys registered for ACPI notifications: {0}" -f $(if ($p.AcpiNotifyRegistered) { 'yes' } else { 'no' })
  "  Studio Brightness++ connected: {0}" -f $(if ($p.ClientConnected) { 'yes' } else { 'no - start the app' })
}

# --- Windows' view --------------------------------------------------------------
Section "Windows' brightness for the display"
$b = Get-StudioBrightness
if (-not $b) {
  '  no WmiMonitorBrightness instance: Windows does not treat the display as having a brightness'
} else {
  "  current {0}%   levels {1}" -f $b.CurrentBrightness, $b.Levels
  $des = Get-Service DisplayEnhancementService -EA SilentlyContinue
  if ($des) {
    "  Display Enhancement Service: {0}{1}" -f $des.Status,
      $(if ($des.Status -eq 'Running') { ' (if the Settings slider is greyed out, restart it elevated: Restart-Service DisplayEnhancementService)' } else { '' })
  }
}

# --- Set ------------------------------------------------------------------------
if ($SetTo -ge 0) {
  Section "WmiSetBrightness($SetTo)"
  $m = Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightnessMethods -EA Stop |
       Where-Object { $_.InstanceName -like 'DISPLAY\APPAE3A*' } | Select-Object -First 1
  if (-not $m) { throw 'no WmiMonitorBrightnessMethods instance for DISPLAY\APPAE3A' }
  try {
    Invoke-CimMethod -InputObject $m -MethodName WmiSetBrightness -Arguments @{ Timeout = [uint32]0; Brightness = [byte]$SetTo } -EA Stop | Out-Null
    '  returned ok'
  } catch { "  FAILED: {0}" -f $_.Exception.Message.Trim() }
  Start-Sleep -Milliseconds 800
  "  current now {0}%" -f (Get-StudioBrightness).CurrentBrightness
}
