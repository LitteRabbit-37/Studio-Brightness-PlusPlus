#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Installs the SbppMonFilter driver package built by build-driver.ps1. Run it elevated.

.DESCRIPTION
  What it changes, in order. Each step says what it is doing as it runs:

    1. Checks that Windows test-signing mode is ON for this boot.
       It does NOT turn it on - that is a boot security setting, and yours.
    2. Creates a System Restore point.
    3. Imports the test certificate (out\SbppMonFilter-test.cer) into
       LocalMachine\Root and LocalMachine\TrustedPublisher, so Windows trusts
       the test-signed package and installs it without a prompt.
    4. Adds the package to the driver store and installs it on the Studio
       Display monitor devnode (pnputil /add-driver /install).
    5. Restarts the Display Enhancement Service. It decides which displays have
       a brightness when it starts, so without this the Settings slider stays
       greyed out until the next reboot.
    6. Shows the result: device status, driver stack, service state.

  Undo all of it with uninstall-dev.ps1 (add -RemoveCertificate to also drop
  the certificate from the machine stores).
#>
param([switch]$SkipRestorePoint)
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg  = Join-Path $here 'out\SbppMonFilter'
$inf  = Join-Path $pkg 'SbppMonFilter.inf'
$cer  = Join-Path $here 'out\SbppMonFilter-test.cer'
foreach ($p in $inf, $cer) {
  if (-not (Test-Path $p)) { throw "missing $p - run build-driver.ps1 first" }
}

function Step([int]$n, [string]$text) {
  Write-Host ''
  Write-Host ("[{0}] {1}" -f $n, $text) -ForegroundColor Cyan
}

# --- 1. Test signing ----------------------------------------------------------
Step 1 'Checking Windows test-signing mode'
$bcd    = (& bcdedit.exe /enum '{current}') -join "`n"
$bcdOn  = $bcd -match '(?m)^testsigning\s+Yes'
$opts   = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions).SystemStartOptions
$bootOn = $opts -match 'TESTSIGNING'
$sb     = try { Confirm-SecureBootUEFI } catch { $null }
Write-Host ("    boot setting: {0}   this boot: {1}   Secure Boot: {2}" -f $(if ($bcdOn) {'on'} else {'off'}),
            $(if ($bootOn) {'test mode'} else {'normal'}), $(if ($null -eq $sb) {'n/a'} elseif ($sb) {'ON'} else {'off'}))
if (-not $bootOn) {
  Write-Host ''
  if ($bcdOn) {
    Write-Host '    Test signing is set but this boot is not in test mode. Reboot, then run this again.' -ForegroundColor Yellow
  } else {
    Write-Host '    Test signing is OFF. This script does not change boot settings.' -ForegroundColor Yellow
    Write-Host '    Turn it on yourself (elevated), reboot, then run this again:'
    Write-Host '        bcdedit /set testsigning on'
    Write-Host '    While it is on Windows shows a "Test Mode" watermark, and games with kernel'
    Write-Host '    anti-cheat may refuse to start. Turn it off again with:'
    Write-Host '        bcdedit /set testsigning off   then reboot (after uninstall-dev.ps1).'
    if ($sb) { Write-Host '    Secure Boot is ON - bcdedit will refuse until it is disabled in firmware.' -ForegroundColor Yellow }
  }
  exit 1
}

# --- 2. Restore point ---------------------------------------------------------
Step 2 'Creating a System Restore point'
if ($SkipRestorePoint) {
  Write-Host '    skipped (-SkipRestorePoint)'
} else {
  $latest = { Get-CimInstance -Namespace root/default -ClassName SystemRestore -EA SilentlyContinue |
              Sort-Object SequenceNumber | Select-Object -Last 1 }
  $before = & $latest
  $r = Invoke-CimMethod -Namespace root/default -ClassName SystemRestore -MethodName CreateRestorePoint `
         -Arguments @{ Description = 'Before SbppMonFilter'; RestorePointType = [uint32]12; EventType = [uint32]100 }
  if ($r.ReturnValue -ne 0) {
    throw ("CreateRestorePoint returned {0}. Check System Protection on C:, or rerun with -SkipRestorePoint." -f $r.ReturnValue)
  }
  $after = & $latest
  if ($after -and (-not $before -or $after.SequenceNumber -ne $before.SequenceNumber)) {
    Write-Host ("    created restore point #{0}: {1}" -f $after.SequenceNumber, $after.Description)
  } elseif ($after) {
    Write-Host ("    Windows created no new point (it allows one per 24 h). Latest is #{0}: {1} ({2})" -f
                $after.SequenceNumber, $after.Description, $after.CreationTime) -ForegroundColor Yellow
  } else {
    Write-Host '    no restore points found - System Protection may be off for C:' -ForegroundColor Yellow
  }
}

# --- 3. Trust the test certificate -------------------------------------------
Step 3 'Trusting the test certificate (LocalMachine Root + TrustedPublisher)'
$c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $cer
Write-Host ("    {0}  thumbprint {1}" -f $c.Subject, $c.Thumbprint)
foreach ($store in 'Root', 'TrustedPublisher') {
  $have = Get-ChildItem ("Cert:\LocalMachine\{0}" -f $store) | Where-Object { $_.Thumbprint -eq $c.Thumbprint }
  if ($have) {
    Write-Host ("    {0}: already present" -f $store)
  } else {
    Import-Certificate -FilePath $cer -CertStoreLocation ("Cert:\LocalMachine\{0}" -f $store) | Out-Null
    Write-Host ("    {0}: imported" -f $store)
  }
}

# --- 4. Install ---------------------------------------------------------------
Step 4 'Installing the package on the Studio Display monitor devnode'
$o = & pnputil.exe /add-driver $inf /install 2>&1
$rc = $LASTEXITCODE
$o | ForEach-Object { Write-Host ("    {0}" -f $_) }
if ($rc -eq 3010) {
  Write-Host '    Windows wants a reboot to finish. Reboot, then run driver-status.ps1.' -ForegroundColor Yellow
} elseif ($rc -ne 0) {
  throw "pnputil /add-driver failed (exit $rc)"
}

# --- 5. Display Enhancement Service ---------------------------------------------
Step 5 'Restarting the Display Enhancement Service (it adopts the display on start)'
$des = Get-Service DisplayEnhancementService -EA SilentlyContinue
if (-not $des) {
  Write-Host '    not present on this Windows build' -ForegroundColor Yellow
} elseif ($des.Status -eq 'Running') {
  Restart-Service DisplayEnhancementService
  Write-Host '    restarted'
} else {
  Write-Host ("    {0} - Windows starts it on demand" -f $des.Status)
}

# --- 6. Result ----------------------------------------------------------------
Step 6 'Result'
$mon = Get-PnpDevice -Class Monitor -PresentOnly | Where-Object { $_.InstanceId -like 'DISPLAY\APPAE3A\*' } | Select-Object -First 1
if (-not $mon) { throw 'Studio Display monitor devnode (DISPLAY\APPAE3A) not found' }
$prop = { param($k) (Get-PnpDeviceProperty -InstanceId $mon.InstanceId -KeyName $k -EA SilentlyContinue).Data }
Write-Host ("    device       : {0}" -f $mon.InstanceId)
Write-Host ("    status       : {0}   problem code {1}" -f $mon.Status, (& $prop 'DEVPKEY_Device_ProblemCode'))
Write-Host ("    driver stack : {0}" -f ((& $prop 'DEVPKEY_Device_Stack') -join ', '))
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='SbppMonFilter'" -EA SilentlyContinue
Write-Host ("    service      : {0}" -f $(if ($svc) { "$($svc.State), start=$($svc.StartMode)" } else { 'not present' }))

Write-Host ''
Write-Host 'Next: start Studio Brightness++, then run driver-status.ps1 (no elevation needed).'
