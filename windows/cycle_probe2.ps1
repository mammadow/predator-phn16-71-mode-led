<#
  cycle_probe2.ps1 - second try at freezing the Mode button's colour cycle.

  From cycle_probe.ps1: 0xA2 (after FD [01, speed] + commit E080) starts the cycle,
  FD speed 00 = ~8 s per round, 09 = ~2.5 s. A bare 0xA0 or 0xA1 does not stop it.

  Tests (cycle started at speed 09 so changes are easy to see):
    A  0xA0 with the FD + commit lead-in       -> stops? on which colour?
    B  FD speed 00 + commit only, no opcode    -> does the running cycle slow down live?
    C  FD speed 00 + commit + 0xA2 again       -> slows down? restarts from the start?
    D  start with speed 20 / 80 / FF           -> seconds per round (very slow = a freeze)
    E  FD first byte 00 / 02 instead of 01     -> any difference?

  Only opcodes already known to be harmless (0xA0, 0xA2, 0xCD) are sent.
  Run from the project folder (no Administrator needed):
    powershell -ExecutionPolicy Bypass -File windows\cycle_probe2.ps1
  Log: windows\cycle_probe2.log
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe  = Join-Path $root 'predator_i2ec.exe'
$log  = Join-Path $PSScriptRoot 'cycle_probe2.log'
Set-Content $log "cycle_probe2 $(Get-Date -Format s)"

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

function Fd([string]$b0, [string]$speed) { return @('w00:FD81', "b03:$b0,$speed", 'w00:E080') }
function Cycle([string]$b0, [string]$speed) { Lt ((Fd $b0 $speed) + 'c01:A2') }

function Test([string]$title, [scriptblock]$action, [string]$question) {
    Log "`n=== $title"
    Restore
    Read-Host "Button should be steady cyan. Press Enter for: $title" | Out-Null
    & $action
    Ask $question
}

Test 'A  fast cycle, then 0xA0 with lead-in' {
    Cycle '01' '09'
    Read-Host '  Cycling fast. Press Enter to send 0xA0 (with FD + commit)' | Out-Null
    Lt ((Fd '01' '09') + 'c01:A0')
} 'Did it stop? On which colour (or back to cyan)? Flash?'

Test 'B  fast cycle, then FD speed 00 + commit, no opcode' {
    Cycle '01' '09'
    Read-Host '  Cycling fast. Press Enter to change the speed to 00 live' | Out-Null
    Lt (Fd '01' '00')
} 'Did the running cycle slow down (to ~8 s per round)? Or no change?'

Test 'C  fast cycle, then FD speed 00 + commit + 0xA2' {
    Cycle '01' '09'
    Read-Host '  Cycling fast. Press Enter to send speed 00 + 0xA2' | Out-Null
    Lt ((Fd '01' '00') + 'c01:A2')
} 'Slowed down? Did it jump back to the start colour (restart), or continue from where it was?'

foreach ($s in '20', '80', 'FF') {
    Test "D  cycle started with speed $s" { Cycle '01' $s } 'Seconds per round? (or: not cycling / stuck on one colour - which?)'
}

foreach ($b in '00', '02') {
    Test "E  cycle started with FD first byte $b (speed 09)" { Cycle $b '09' } 'Any difference from the normal fast cycle? (not cycling / other colours / brightness)'
}

Log "`n=== restore"
Restore
Log "done. Log: $log"
