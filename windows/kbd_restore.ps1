<#
  kbd_restore.ps1 - put the keyboard lighting back to what device_probe.ps1 read
  before its test: effect 3 (wave), speed 3, brightness 100, direction 1, device 1.
  (GetGamingKBBacklight(1) = 03 03 64 00 01 00 00 00 03 at 2026-11-01 00:03.)

  The device-3 call in device_probe.ps1 left the BIOS record at static with black
  zone colours, which is why the keyboard went dark.

  Run from an Administrator PowerShell in the project folder:
    powershell -ExecutionPolicy Bypass -File windows\kbd_restore.ps1
#>
$ErrorActionPreference = 'Stop'
$inst = Get-CimInstance -Namespace root/wmi -ClassName AcerGamingFunction | Select-Object -First 1

function Hex($v) { if ($v -is [array]) { ($v | ForEach-Object { '{0:X2}' -f $_ }) -join ' ' } else { '0x{0:X}' -f $v } }

function Record([int]$device) {
    $r = Invoke-CimMethod -InputObject $inst -MethodName GetGamingKBBacklight -Arguments @{ gmInput = [uint32]$device }
    "  device $device record: [$(Hex $r.gmOutput)] status $(Hex $r.gmReturn)"
}

function SetEffect([int]$device) {
    $a = [byte[]]@(3, 3, 100, 0, 1, 0, 0, 0, 3, $device, 0, 0, 0, 0, 0, 0)
    $r = Invoke-CimMethod -InputObject $inst -MethodName SetGamingKBBacklight -Arguments @{ gmInput = $a }
    "SetGamingKBBacklight device $device [$(Hex $a)] -> $(Hex $r.gmOutput)"
}

'Before:'; Record 1; Record 3
SetEffect 1
Start-Sleep -Milliseconds 800
'After:'; Record 1; Record 3
$r3 = Invoke-CimMethod -InputObject $inst -MethodName GetGamingKBBacklight -Arguments @{ gmInput = [uint32]3 }
if ($r3.gmOutput[0] -ne 3) {
    'Device 3 (global) still differs; setting it too.'
    SetEffect 3
    Start-Sleep -Milliseconds 800
    Record 1; Record 3
}
"`nThe keyboard should show the wave effect again."
