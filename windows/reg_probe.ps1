<#
  reg_probe.ps1 - one-off experiment on the Mode-button LED controller (SMBus 0x67):
  can the 0xCD preset flash be cut short by sending another opcode right after it?

    0xCD  apply Mode preset (always plays the flash on button + keyboard)
    0xA0  button "static" effect (0xA0 + mode family: 0xA1 breathing, 0xA2 cycle)
    0xC2  keyboard static zones

  All three opcodes are already known to be harmless on this machine; only the
  back-to-back order is new.

  Run from the project folder (no Administrator needed):
    powershell -ExecutionPolicy Bypass -File windows\reg_probe.ps1
  Watch the Mode button. After each step, type what you saw.
  Everything goes to windows\reg_probe.log.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe  = Join-Path $root 'predator_i2ec.exe'
$log  = Join-Path $PSScriptRoot 'reg_probe.log'
Set-Content $log "reg_probe $(Get-Date -Format s)"

function Log([string]$s) { Write-Host $s; Add-Content $log $s }

function Run([string[]]$a) {
    Log "> predator_i2ec $($a -join ' ')"
    & $exe @a 2>&1 | ForEach-Object { Log "    $_" }
}

$preset = @('w00:FC81', 'b03:XX', 'w00:E080', 'c01:CD')
function Preset([string]$code, [string[]]$after) { return ($preset -replace 'XX', $code) + $after }

$steps = @(
    @{ name = 'A  normal change to GREEN (0xCD only) - note how long the flash is'; lt = (Preset '01' @()) },
    @{ name = 'B  RED via 0xCD, then 0xA0 immediately';                            lt = (Preset '07' @('c01:A0')) },
    @{ name = 'C  PURPLE via 0xCD, then 0xA0 and 0xC2 immediately';                 lt = (Preset '04' @('c01:A0', 'c01:C2')) },
    @{ name = 'D  ORANGE via 0xCD, then 0xA0 three times';                          lt = (Preset '06' @('c01:A0', 'c01:A0', 'c01:A0')) }
)

foreach ($s in $steps) {
    Read-Host "`nLook at the Mode button. Press Enter for step $($s.name)" | Out-Null
    Log "=== $($s.name)"
    Run (@('--lt') + $s.lt + '--yes')
    $a = Read-Host '  Flash compared to step A: same / shorter / none? Final colour?'
    Log "  you saw: $a"
}

Log '=== restore'
Run @('--led-restore', '--yes')
Log "done. Log: $log"
