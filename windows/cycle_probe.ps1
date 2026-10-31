<#
  cycle_probe.ps1 - can the Mode button's colour cycle be frozen on a colour?

  Known: 0xA0-0xA3 sent together start an automatic colour cycle on the button
  (no flash); 0xA1 alone = breathing in the current colour; 0xCD (--led-restore)
  stops it. Button effects look like the keyboard's 0xC2 + mode family:
  0xA0 static, 0xA1 breathing, 0xA2 neon/cycle, ...

  Questions:
    1. Does 0xA2 alone start the cycle?
    2. While cycling, does 0xA0 (static) freeze the colour shown at that moment?
    3. While cycling, does 0xA1 breathe in the cycled colour?
    4. Does the page FD speed byte change the cycle speed?

  Only opcodes already known to be harmless (0xA0-0xA3, 0xCD) are sent.
  Run from the project folder (no Administrator needed):
    powershell -ExecutionPolicy Bypass -File windows\cycle_probe.ps1
  Log: windows\cycle_probe.log
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe  = Join-Path $root 'predator_i2ec.exe'
$log  = Join-Path $PSScriptRoot 'cycle_probe.log'
Set-Content $log "cycle_probe $(Get-Date -Format s)"

function Log([string]$s) { Write-Host $s; Add-Content $log $s }

function Lt([string[]]$steps) {
    Log "> --lt $($steps -join ' ')"
    $out = & $exe --lt @steps --yes 2>&1
    if ($out -match 'FAIL') { $out | ForEach-Object { Log "    $_" }; Log '!!! transaction failed - stopping'; & $exe --led-restore --yes | Out-Null; exit 1 }
    Log "    sent $(Get-Date -Format 'HH:mm:ss.fff')"
}

function Ask([string]$q) { $a = Read-Host "  $q"; Log "  you said: $a"; return $a.Trim().ToLower() }

function Restore {
    Log '> --led-restore'
    & $exe --led-restore --yes | Out-Null
    Start-Sleep -Seconds 3
}

$cycleAll = @('w00:FD81', 'b03:01,00', 'w00:E080', 'c01:A0', 'c01:A1', 'c01:A2', 'c01:A3')

# Start the cycle; returns the step list that worked.
function StartCycle {
    if ($script:cycle) { Lt $script:cycle; return }
    Lt @('w00:FD81', 'b03:01,00', 'w00:E080', 'c01:A2')
    if ((Ask 'Is the button cycling through colours now? (y/n)').StartsWith('y')) {
        $script:cycle = @('w00:FD81', 'b03:01,00', 'w00:E080', 'c01:A2'); return
    }
    Log '  A2 alone did not cycle; trying A0-A3 together'
    Lt $cycleAll
    if ((Ask 'Is it cycling now? (y/n)').StartsWith('y')) { $script:cycle = $cycleAll; return }
    Log 'RESULT: could not start the cycle'
    Restore; exit 0
}
$script:cycle = $null

Log '=== 1. start the cycle'
Restore
Read-Host 'Button should be steady cyan. Press Enter to start the cycle, then watch it' | Out-Null
StartCycle
Ask 'Describe the cycle: which colours, how long one full round takes (seconds), does brightness change too?' | Out-Null

Log '=== 2. freeze with 0xA0 (static)'
Read-Host 'Let it cycle. Press Enter when it shows a colour you can name (it may drift ~1 s before the command lands)' | Out-Null
Log "  Enter pressed $(Get-Date -Format 'HH:mm:ss.fff')"
Lt @('c01:A0')
Ask 'What happened? frozen on a colour (which)? back to cyan? still cycling? flash?' | Out-Null

Log '=== 3. breathing with 0xA1 during the cycle'
Restore
Read-Host 'Press Enter to start the cycle again' | Out-Null
StartCycle
Read-Host 'Press Enter when it shows a colour you can name' | Out-Null
Log "  Enter pressed $(Get-Date -Format 'HH:mm:ss.fff')"
Lt @('c01:A1')
Ask 'What happened? breathing in which colour? still cycling?' | Out-Null

Log '=== 4. cycle speed (page FD byte 2 = 09 instead of 00)'
Restore
Read-Host 'Press Enter to start the cycle with speed byte 09' | Out-Null
$fast = @($script:cycle | ForEach-Object { if ($_ -eq 'b03:01,00') { 'b03:01,09' } else { $_ } })
Lt $fast
Ask 'Compared to before: faster, slower, or the same? (seconds per round if you can tell)' | Out-Null

Log '=== restore'
Restore
Log "done. Log: $log"
