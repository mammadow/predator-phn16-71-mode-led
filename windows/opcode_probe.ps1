<#
  opcode_probe.ps1 - try the execute opcodes the Linux sweeps skipped, one at a time:
    0xA4-0xAF  continuation of the button effect family (0xA0 static, 0xA1 breathing, 0xA2 cycle)
    0xB0-0xB3, 0x94-0x9F  the rest of the untested gap

  Before each opcode the chip gets every colour source we know of, all RED:
    page FC = preset 7 (red)   page F2 = keyboard zones FF0000 x4, brightness 100
    page FD = 01 00            commit E080
  so an opcode that applies a colour without the 0xCD flash shows up as red.

  RISK: these opcodes are unknown. The README's own warning applies: an unknown
  opcode could do anything to the lighting chip. The neighbouring opcodes were all
  harmless effects, but that is no guarantee. If the LED or keyboard acts strangely:
  switch modes once; if that doesn't help, shut down fully and power on again.
  The keyboard may turn red; set your lighting again in PredatorSense afterwards.

  Run from the project folder (no Administrator needed):
    powershell -ExecutionPolicy Bypass -File windows\opcode_probe.ps1
  Type q at any prompt to stop early. Log: windows\opcode_probe.log
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe  = Join-Path $root 'predator_i2ec.exe'
$log  = Join-Path $PSScriptRoot 'opcode_probe.log'
Set-Content $log "opcode_probe $(Get-Date -Format s)"

function Log([string]$s) { Write-Host $s; Add-Content $log $s }

function Restore {
    & $exe --led-restore --yes | Out-Null
    Start-Sleep -Seconds 3
}

$prep = @('w00:FC81', 'b03:07',
          'w00:F280', 'b03:FF,00,00,FF,00,00,FF,00,00,FF,00,00,64',
          'w00:FD81', 'b03:01,00', 'w00:E080')

$ops = @(0xA4..0xAF) + @(0xB0..0xB3) + @(0x94..0x9F)
foreach ($o in $ops) {
    $op = '{0:X2}' -f $o
    Log "`n=== opcode 0x$op"
    Restore
    $r = Read-Host "Button should be steady cyan. Press Enter to send opcode 0x$op (q = stop)"
    if ($r -eq 'q') { Log '  stopped by you'; break }
    $out = & $exe --lt @prep "c01:$op" --yes 2>&1
    if ($out -match 'FAIL') { $out | ForEach-Object { Log "    $_" }; Log '!!! the chip did not ACK - stopping'; break }
    Log "  sent $(Get-Date -Format 'HH:mm:ss.fff')"
    $a = Read-Host '  Button: colour / effect / flash? Keyboard? (Enter = nothing, q = stop)'
    Log "  you said: $a"
    if ($a -eq 'q') { break }
}

Log "`n=== restore"
Restore
Log "done. Log: $log"
