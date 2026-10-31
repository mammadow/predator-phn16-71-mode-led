<#
  led_probe.ps1 - one-off experiment: does the BIOS LED-group WMI interface
  (methods 10-13, used by Acer's AcerECLightbarController.dll for the logo LED)
  reach the Mode button on the PHN16-71?

  Decoded from AcerECLightbarController.dll (AcerECLogoLEDController::SendColors):
    SetGamingLEDColor    u64 = [group, R, G, B, brightness 0-100, speed]
    SetGamingLEDBehavior u64 = [group, 0, effect]   (0x10001 = group 1, static)
  Logo = group 1. Groups 2 and 4 are guesses (bitmask pattern).

  Run from an Administrator PowerShell in the project folder:
    powershell -ExecutionPolicy Bypass -File windows\led_probe.ps1
  Watch the Mode button and the keyboard. After each step, type what you saw.
  Everything goes to windows\led_probe.log (EC bus log: windows\led_probe_sniff.txt).
#>
$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot
$exe   = Join-Path $root 'predator_i2ec.exe'
$log   = Join-Path $PSScriptRoot 'led_probe.log'
$sniff = Join-Path $PSScriptRoot 'led_probe_sniff.txt'
Set-Content $log "led_probe $(Get-Date -Format s)"

$inst = Get-CimInstance -Namespace root/wmi -ClassName AcerGamingFunction | Select-Object -First 1
$t0 = Get-Date

function Log([string]$s) {
    $line = '{0,8:N3}  {1}' -f ((Get-Date) - $t0).TotalSeconds, $s
    Write-Host $line
    Add-Content $log $line
}

function Call([string]$method, $arg) {
    try {
        $r = Invoke-CimMethod -InputObject $inst -MethodName $method -Arguments @{ gmInput = $arg }
        $outs = foreach ($p in $r.PSObject.Properties) {
            if ($p.Name -in 'ReturnValue', 'PSComputerName') { continue }
            if ($p.Value -is [array]) { "$($p.Name)=[" + (($p.Value | ForEach-Object { '{0:X2}' -f $_ }) -join ' ') + ']' }
            else { "$($p.Name)=0x{0:X}" -f $p.Value }
        }
        Log ("{0}(0x{1:X}) -> {2}" -f $method, $arg, ($outs -join ', '))
    } catch {
        Log ("{0}(0x{1:X}) FAILED: {2}" -f $method, $arg, $_.Exception.Message)
    }
}

function Ask([string]$q) {
    $a = Read-Host "  $q"
    Log "  you saw: $a"
}

# [group, R, G, B, brightness]
function ColorArg([int]$group, [string]$rgb, [int]$bright) {
    $v = [Convert]::ToUInt32($rgb, 16)
    $r = ($v -shr 16) -band 0xFF; $g = ($v -shr 8) -band 0xFF; $b = $v -band 0xFF
    return [uint64]$group -bor ([uint64]$r -shl 8) -bor ([uint64]$g -shl 16) -bor ([uint64]$b -shl 24) -bor ([uint64]$bright -shl 32)
}

Log '=== 1. read-only queries ==='
foreach ($id in 1, 2, 4, 8) {
    Call GetGamingLED         ([uint32]$id)
    Call GetGamingLEDBehavior ([uint32]$id)
    Call GetGamingLEDColor    ([uint32]$id)
}

Log '=== 2. colour calls (EC bus sniffer running) ==='
$sn = Start-Process -FilePath $exe -ArgumentList '--sniff', '900' -RedirectStandardOutput $sniff -NoNewWindow -PassThru
Start-Sleep -Milliseconds 1500
Log "sniffer started (pid $($sn.Id)); its t=0 is about 1.5 s before this line"

$steps = @(
    @{ g = 1; rgb = 'FF0000'; name = 'group 1 (logo), RED' },
    @{ g = 1; rgb = '00FF00'; name = 'group 1 (logo), GREEN' },
    @{ g = 1; rgb = '0000FF'; name = 'group 1 (logo), BLUE' },
    @{ g = 2; rgb = 'FF0000'; name = 'group 2, RED' },
    @{ g = 4; rgb = 'FF0000'; name = 'group 4, RED' }
)
foreach ($s in $steps) {
    Read-Host "`nLook at the Mode button. Press Enter to send: $($s.name)" | Out-Null
    Log "--- $($s.name)"
    Call SetGamingLEDColor    (ColorArg $s.g $s.rgb 100)
    Call SetGamingLEDBehavior ([uint64]$s.g -bor 0x10000)
    Start-Sleep -Milliseconds 1500
    Call GetGamingLEDColor    ([uint32]$s.g)
    Call GetGamingLEDBehavior ([uint32]$s.g)
    Ask 'What changed? (Mode button colour? flash? keyboard? nothing?)'
}

Log '=== 3. restore ==='
Stop-Process -Id $sn.Id -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
$out = & $exe --led-restore --yes 2>&1
$out | ForEach-Object { Log "  $_" }
Log "done. Log: $log"
Write-Host "`nIf the keyboard colour changed, reopen PredatorSense (or set it there) to put it back."
