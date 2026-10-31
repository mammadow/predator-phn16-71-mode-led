<#
  device_probe.ps1 - does the keyboard-effect WMI method reach the Mode button
  through its "device" byte?

  SetGamingKBBacklight (method 20), layout from the PHN16-72 project's notes
  (github.com/aishend/predator-sense-phn16-72-linux, docs/REVERSE_ENGINEERING.md):
    [effect, speed 1-9, brightness 0-100, 0, direction, R, G, B, 3, device, 0 x6]
    device: 1 keyboard, 2 light bar, 3 all/global
  The PHN16-71 has no light bar, and only device 1 was ever tried here.

    1. read-only: GetGamingKBBacklight(1/2/3), GetGamingLED(1/2/4/8)
    2. with the EC bus sniffer running: static RED then GREEN to device 2, then device 3

  Run from an Administrator PowerShell in the project folder:
    powershell -ExecutionPolicy Bypass -File windows\device_probe.ps1
  The keyboard may change colour; set your lighting again in PredatorSense afterwards.
  Log: windows\device_probe.log (EC bus log: windows\device_probe_sniff.txt)
#>
$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot
$exe   = Join-Path $root 'predator_i2ec.exe'
$log   = Join-Path $PSScriptRoot 'device_probe.log'
$sniff = Join-Path $PSScriptRoot 'device_probe_sniff.txt'
Set-Content $log "device_probe $(Get-Date -Format s)"

$inst = Get-CimInstance -Namespace root/wmi -ClassName AcerGamingFunction | Select-Object -First 1
$t0 = Get-Date

function Log([string]$s) {
    $line = '{0,8:N3}  {1}' -f ((Get-Date) - $t0).TotalSeconds, $s
    Write-Host $line
    Add-Content $log $line
}

function Show($v) {
    if ($v -is [array]) { return '[' + (($v | ForEach-Object { '{0:X2}' -f $_ }) -join ' ') + ']' }
    return '0x{0:X}' -f $v
}

function Call([string]$method, $arg) {
    try {
        $r = Invoke-CimMethod -InputObject $inst -MethodName $method -Arguments @{ gmInput = $arg }
        $outs = foreach ($p in $r.PSObject.Properties) {
            if ($p.Name -in 'ReturnValue', 'PSComputerName') { continue }
            "$($p.Name)=$(Show $p.Value)"
        }
        Log "$method($(Show $arg)) -> $($outs -join ', ')"
    } catch {
        Log "$method($(Show $arg)) FAILED: $($_.Exception.Message)"
    }
}

function Ask([string]$q) { $a = Read-Host "  $q"; Log "  you saw: $a" }

# static effect, full brightness, one colour, given device
function KbArgs([string]$rgb, [int]$device) {
    $v = [Convert]::ToUInt32($rgb, 16)
    # leading comma: return the byte[] as one object instead of unrolling it to Object[]
    return , [byte[]]@(0, 1, 100, 0, 1, (($v -shr 16) -band 0xFF), (($v -shr 8) -band 0xFF), ($v -band 0xFF), 3, $device, 0, 0, 0, 0, 0, 0)
}

Log '=== 1. read-only queries'
foreach ($d in 1, 2, 3) { Call GetGamingKBBacklight ([uint32]$d) }
foreach ($d in 1, 2, 4, 8) { Call GetGamingLED ([uint32]$d) }

Log '=== 2. colour calls (EC bus sniffer running)'
$sn = Start-Process -FilePath $exe -ArgumentList '--sniff', '900' -RedirectStandardOutput $sniff -NoNewWindow -PassThru
Start-Sleep -Milliseconds 1500
Log "sniffer started (pid $($sn.Id)); its t=0 is about 1.5 s before this line"

foreach ($d in 2, 3) {
    foreach ($c in @(@('FF0000', 'RED'), @('00FF00', 'GREEN'))) {
        Read-Host "`nWatch the Mode button and keyboard. Press Enter to send static $($c[1]) to device $d" | Out-Null
        Log "--- device $d, $($c[1])"
        Call SetGamingKBBacklight (KbArgs $c[0] $d)
        Start-Sleep -Milliseconds 1500
        Call GetGamingKBBacklight ([uint32]$d)
        Ask 'What changed? (Mode button colour? flash? keyboard? nothing?)'
    }
}

Log '=== 3. restore'
Stop-Process -Id $sn.Id -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
& $exe --led-restore --yes 2>&1 | ForEach-Object { Log "  $_" }
Log "done. Log: $log"
Write-Host "`nIf the keyboard colour changed, set your lighting again in PredatorSense."
