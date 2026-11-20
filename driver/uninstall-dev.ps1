#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Removes SbppMonFilter from this PC. Run it elevated - locally, or over an
  elevated remote shell (e.g. SSH) if the local display is unusable.

.DESCRIPTION
    1. Removes every SbppMonFilter package from the driver store and from the
       Studio Display monitor devnode (pnputil /delete-driver /uninstall).
    2. Deletes "BrightnessControl" from the monitor's software key if it is
       still there (the package sets it; Windows does not reliably clean
       up values an extension INF wrote).
    3. Restarts the monitor devnode and checks its real driver stack. If
       SbppMonFilter is still in it and listed in the legacy UpperFilters or
       LowerFilters values, removes it there through SetupAPI and restarts
       again.
    4. Deletes the SbppMonFilter service if anything is left of it.
    5. With -RemoveCertificate, removes the test certificate from
       LocalMachine\Root and LocalMachine\TrustedPublisher.
    6. Restarts the Display Enhancement Service, so Windows drops the
       brightness slider it showed for the display.

  It does not touch test-signing mode. Turn that off yourself afterwards:
      bcdedit /set testsigning off   then reboot.
#>
param([switch]$RemoveCertificate)
$ErrorActionPreference = 'Stop'

$certSubject  = 'CN=SbppMonFilter Test Signing'
$monitorClass = [Guid]'4d36e96e-e325-11ce-bfc1-08002be10318'

function Step([int]$n, [string]$text) {
  Write-Host ''
  Write-Host ("[{0}] {1}" -f $n, $text) -ForegroundColor Cyan
}
function Get-StudioMonitor {
  Get-PnpDevice -Class Monitor -PresentOnly | Where-Object { $_.InstanceId -like 'DISPLAY\APPAE3A\*' } | Select-Object -First 1
}
function Get-Prop([string]$id, [string]$key) {
  (Get-PnpDeviceProperty -InstanceId $id -KeyName $key -EA SilentlyContinue).Data
}
function Restart-Monitor([string]$id) {
  & pnputil.exe /restart-device $id | Out-Null
  Start-Sleep -Seconds 2
}
function Show-Monitor([string]$id) {
  $m = Get-PnpDevice -InstanceId $id -EA SilentlyContinue
  Write-Host ("    status       : {0}   problem code {1}" -f $m.Status, (Get-Prop $id 'DEVPKEY_Device_ProblemCode'))
  Write-Host ("    driver stack : {0}" -f ((Get-Prop $id 'DEVPKEY_Device_Stack') -join ', '))
}
function Get-SoftwareKey([string]$id, [bool]$writable) {
  $drv = Get-Prop $id 'DEVPKEY_Device_Driver'
  if (-not $drv) { return $null }
  [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("SYSTEM\CurrentControlSet\Control\Class\$drv", $writable)
}

# --- 1. Driver store ----------------------------------------------------------
Step 1 'Removing SbppMonFilter packages'
$published = @()
$current = $null
foreach ($line in (& pnputil.exe /enum-drivers)) {
  if ($line -match '^\s*Published Name:\s*(\S+)') { $current = $Matches[1] }
  elseif ($line -match '^\s*Original Name:\s*(\S+)' -and $Matches[1] -ieq 'sbppmonfilter.inf' -and $current) {
    $published += $current
  }
}
if (-not $published) { Write-Host '    none in the driver store' }
foreach ($p in $published) {
  Write-Host ("    deleting {0}" -f $p)
  & pnputil.exe /delete-driver $p /uninstall /force | ForEach-Object { Write-Host ("      {0}" -f $_) }
}

$mon = Get-StudioMonitor
if (-not $mon) {
  Write-Host ''
  Write-Host 'DISPLAY\APPAE3A is not present right now - nothing more to clean on the device.' -ForegroundColor Yellow
} else {
  # --- 2. BrightnessControl ---------------------------------------------------
  Step 2 'BrightnessControl in the monitor software key'
  $changed = $false
  $rk = Get-SoftwareKey $mon.InstanceId $true
  if (-not $rk) {
    Write-Host '    software key not found'
  } else {
    try {
      if ($null -ne $rk.GetValue('BrightnessControl')) {
        $rk.DeleteValue('BrightnessControl')
        Write-Host '    removed'
        $changed = $true
      } else {
        Write-Host '    not present'
      }
    } finally { $rk.Close() }
  }

  # --- 3. Driver stack ----------------------------------------------------------
  Step 3 'Checking the monitor driver stack'
  Restart-Monitor $mon.InstanceId
  $stack = @(Get-Prop $mon.InstanceId 'DEVPKEY_Device_Stack')
  if ($stack -contains '\Driver\SbppMonFilter') {
    Write-Host '    SbppMonFilter is still in the stack - checking the legacy filter values' -ForegroundColor Yellow
    if (-not ('SbppDevReg' -as [type])) {
      Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class SbppDevReg {
    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevsW(ref Guid classGuid, string enumerator, IntPtr hwnd, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetDeviceInstanceIdW(IntPtr set, ref SP_DEVINFO_DATA data, StringBuilder id, int size, out int required);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, uint prop, out uint regType, byte[] buf, uint size, out uint required);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, uint prop, byte[] buf, uint size);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    const uint DIGCF_PRESENT = 0x2;
    const uint SPDRP_UPPERFILTERS = 0x11;
    const uint SPDRP_LOWERFILTERS = 0x12;

    // Removes 'filter' from UpperFilters or LowerFilters; returns what remains, or null if the device was not found.
    public static string[] RemoveFilter(Guid classGuid, string instanceId, string filter, bool upper) {
        uint prop = upper ? SPDRP_UPPERFILTERS : SPDRP_LOWERFILTERS;
        IntPtr set = SetupDiGetClassDevsW(ref classGuid, null, IntPtr.Zero, DIGCF_PRESENT);
        if (set == new IntPtr(-1)) throw new Win32Exception();
        try {
            for (uint i = 0; ; i++) {
                var d = new SP_DEVINFO_DATA();
                d.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
                if (!SetupDiEnumDeviceInfo(set, i, ref d)) break;
                var sb = new StringBuilder(512);
                int req;
                if (!SetupDiGetDeviceInstanceIdW(set, ref d, sb, sb.Capacity, out req)) continue;
                if (!string.Equals(sb.ToString(), instanceId, StringComparison.OrdinalIgnoreCase)) continue;

                var current = new List<string>();
                uint type, need;
                var buf = new byte[4096];
                if (SetupDiGetDeviceRegistryPropertyW(set, ref d, prop, out type, buf, (uint)buf.Length, out need)) {
                    foreach (var s in Encoding.Unicode.GetString(buf, 0, (int)need).Split('\0'))
                        if (s.Length > 0) current.Add(s);
                }
                var keep = current.FindAll(s => !string.Equals(s, filter, StringComparison.OrdinalIgnoreCase));
                if (keep.Count == current.Count) return keep.ToArray();
                bool ok;
                if (keep.Count == 0) {
                    ok = SetupDiSetDeviceRegistryPropertyW(set, ref d, prop, null, 0);
                } else {
                    var bytes = Encoding.Unicode.GetBytes(string.Join("\0", keep) + "\0\0");
                    ok = SetupDiSetDeviceRegistryPropertyW(set, ref d, prop, bytes, (uint)bytes.Length);
                }
                if (!ok) throw new Win32Exception();
                return keep.ToArray();
            }
            return null;
        } finally {
            SetupDiDestroyDeviceInfoList(set);
        }
    }
}
'@
    }
    foreach ($which in 'Upper', 'Lower') {
      $f = @(Get-Prop $mon.InstanceId ("DEVPKEY_Device_{0}Filters" -f $which))
      if ($f -contains 'SbppMonFilter') {
        $left = [SbppDevReg]::RemoveFilter($monitorClass, $mon.InstanceId, 'SbppMonFilter', ($which -eq 'Upper'))
        Write-Host ("    removed from {0}Filters; left: {1}" -f $which, $(if ($left) { $left -join ', ' } else { '(none)' }))
        $changed = $true
      }
    }
  }
  if ($changed) { Restart-Monitor $mon.InstanceId }
  Show-Monitor $mon.InstanceId
  $rk = Get-SoftwareKey $mon.InstanceId $false
  if ($rk) {
    try { Write-Host ("    BrightnessControl: {0}" -f $(if ($null -ne $rk.GetValue('BrightnessControl')) { $rk.GetValue('BrightnessControl') } else { 'absent' })) }
    finally { $rk.Close() }
  }
}

# --- 4. Service -----------------------------------------------------------------
Step 4 'Checking for a leftover SbppMonFilter service'
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='SbppMonFilter'" -EA SilentlyContinue
if ($svc) {
  if ($svc.State -eq 'Running') {
    Write-Host '    service is still running - reboot, then run this again to delete it' -ForegroundColor Yellow
  } else {
    & sc.exe delete SbppMonFilter | ForEach-Object { Write-Host ("    {0}" -f $_) }
  }
} else {
  Write-Host '    none'
}

# --- 5. Certificate -------------------------------------------------------------
Step 5 'Test certificate'
foreach ($store in 'Root', 'TrustedPublisher') {
  $certs = Get-ChildItem ("Cert:\LocalMachine\{0}" -f $store) | Where-Object { $_.Subject -eq $certSubject }
  if (-not $certs) { Write-Host ("    {0}: not present" -f $store); continue }
  if ($RemoveCertificate) {
    $x = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
    $x.Open('ReadWrite')
    try {
      foreach ($cert in $certs) {
        $x.Remove($cert)
        Write-Host ("    {0}: removed {1}" -f $store, $cert.Thumbprint)
      }
    } finally { $x.Close() }
  } else {
    Write-Host ("    {0}: still trusted ({1}) - rerun with -RemoveCertificate to drop it" -f $store, ($certs.Thumbprint -join ', '))
  }
}

# --- 6. Display Enhancement Service -----------------------------------------------
Step 6 'Restarting the Display Enhancement Service'
$des = Get-Service DisplayEnhancementService -EA SilentlyContinue
if ($des -and $des.Status -eq 'Running') { Restart-Service DisplayEnhancementService; Write-Host '    restarted' }
else { Write-Host '    not running - nothing to do' }

Write-Host ''
Write-Host 'Done. Test signing is unchanged; turn it off with: bcdedit /set testsigning off   then reboot.'
