<#
  static_probe.ps1 - does 0xA0 (button static) pick up a new colour without the flash?

  From cycle_probe2.ps1: FD [01, speed] + commit E080 + 0xA0 stops the colour cycle
  and shows the preset colour again WITHOUT a flash. Older tests sent the preset
  page (FC) + commit + 0xA0 without the FD lead-in and saw no change.

    1  FC preset 7 (red)    + FD + commit + 0xA0      from steady cyan
    2  FC preset 1 (green)  + FD + commit + 0xA0      repeat with another colour
    3  FC preset 4 (purple) + FD + commit + 0xA1      breathing in the new preset?
    4  cycle running, then FC preset 6 (orange) + FD + commit + 0xA0
    5  F2 block of 16 bytes (keyboard zones in stock cyan + FF 00 00 extra) + FD + commit + 0xA0
    6  FC block [00 FF 00 00] + FD + commit + 0xA0    (preset byte 0 = off, then RGB red)

  Only opcodes already known to be harmless (0xA0-0xA2, 0xCD) are sent.
  Run from the project folder (no Administrator needed):
    powershell -ExecutionPolicy Bypass -File windows\static_probe.ps1
  Log: windows\static_probe.log
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe  = Join-Path $root 'predator_i2ec.exe'
$log  = Join-Path $PSScriptRoot 'static_probe.log'
Set-Content $log "static_probe $(Get-Date -Format s)"

function Log([string]$s) { Write-Host $s; Add-Content $log $s }

function Lt([string[]]$steps) {
    Log "> --lt $($steps -join ' ')"
    $out = & $exe --lt @steps --yes 2>&1
    if ($out -match 'FAIL') { $out | ForEach-Object { Log "    $_" }; Log '!!! transaction failed - stopping'; & $exe --led-restore --yes | Out-Null; exit 1 }
    Log "    sent $(Get-Date -Format 'HH:mm:ss.fff')"
}

function Ask([string]$q) { $a = Read-Host "  $q"; Log "  you said: $a" }

function Restore {
    Log '> --led-restore'
    & $exe --led-restore --yes | Out-Null
    Start-Sleep -Seconds 3
}

$lead = @('w00:FD81', 'b03:01,00', 'w00:E080')
function PresetPage([string]$data) { return @('w00:FC81', "b03:$data") }

function Test([string]$title, [scriptblock]$action, [string]$question) {
    Log "`n=== $title"
    Restore
    Read-Host "Button should be steady cyan. Press Enter for: $title" | Out-Null
    & $action
    Ask $question
}

Test '1  preset RED via 0xA0' { Lt ((PresetPage '07') + $lead + 'c01:A0') } 'Did it turn red? With or without the flash? (or no change)'
Test '2  preset GREEN via 0xA0' { Lt ((PresetPage '01') + $lead + 'c01:A0') } 'Did it turn green? Flash?'
Test '3  preset PURPLE via 0xA1' { Lt ((PresetPage '04') + $lead + 'c01:A1') } 'Breathing in which colour? Flash?'
Test '4  cycle, then preset ORANGE via 0xA0' {
    Lt ($lead + 'c01:A2')
    Read-Host '  Cycling. Press Enter to send preset orange + 0xA0' | Out-Null
    Lt ((PresetPage '06') + $lead + 'c01:A0')
} 'Stopped on orange, on cyan, or still cycling? Flash?'
Test '5  16-byte F2 block + 0xA0' {
    Lt (@('w00:F280', 'b03:00,AE,C7,00,AE,C7,00,AE,C7,00,AE,C7,64,FF,00,00') + $lead + 'c01:A0')
} 'Did the button change colour? Did the keyboard change?'
Test '6  FC block [00 FF 00 00] + 0xA0' { Lt ((PresetPage '00,FF,00,00') + $lead + 'c01:A0') } 'Button off, red, cyan, something else? Flash?'

Log "`n=== restore"
Restore
Log "done. Log: $log"
