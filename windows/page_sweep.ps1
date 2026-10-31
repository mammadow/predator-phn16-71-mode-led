<#
  page_sweep.ps1 - look for a writable Mode-button colour store on the LED
  controller (SMBus 0x67), i.e. RGB without the 0xCD preset flash.

  Known pages: F2 keyboard zones, FC mode preset, FD effect params, E0 commit.
  This writes an RGB pattern (FF0000 x4 + brightness 100, the page-F2 layout) into
  the 13 untried pages F0 F1 F3-FB FE FF, commits (E080) and fires a button effect
  opcode: 0xA0 static, 0xA1 breathing, or 0xC2 static apply.

    Phase 1  all pages at once, each opcode (page low byte 0x80, then 0x81)
    Phase 2  if one makes the button change, halve the page list until one is left
    Phase 3  confirm that page with green and blue

  Before every round, --led-restore puts the button back to stock cyan (one flash).
  Nothing here sends a save/flash-write command, but what these pages do is unknown.
  If the LED or keyboard acts strangely afterwards: switch modes once; if that
  doesn't help, shut down fully (not restart) and power on again.

  Run from the project folder (no Administrator needed):
    powershell -ExecutionPolicy Bypass -File windows\page_sweep.ps1
  Log: windows\page_sweep.log
#>
param([switch]$DryRun)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe  = Join-Path $root 'predator_i2ec.exe'
$log  = Join-Path $PSScriptRoot $(if ($DryRun) { 'page_sweep_dryrun.log' } else { 'page_sweep.log' })
Set-Content $log "page_sweep $(Get-Date -Format s)$(if ($DryRun) { ' DRY RUN' })"

function Log([string]$s) { Write-Host $s; Add-Content $log $s }

function Tool([string[]]$a) {
    Log "> predator_i2ec $($a -join ' ')"
    if ($DryRun) { return }
    $out = & $exe @a 2>&1
    $out | ForEach-Object { Log "    $_" }
    if ($out -match 'FAIL') { Log '!!! a transaction failed - stopping here'; Tool @('--led-restore', '--yes'); exit 1 }
}

function Pattern([string]$rgb) {
    $c = @($rgb.Substring(0, 2), $rgb.Substring(2, 2), $rgb.Substring(4, 2))
    return (($c + $c + $c + $c) + '64') -join ','
}

# Write $pages with colour $rgb, commit, fire $op; ask whether the button changed.
function Round([string[]]$pages, [string]$lo, [string]$op, [string]$rgb, [string]$what) {
    Log "`n=== round: pages [$($pages -join ' ')] low byte $lo, opcode $op, colour $rgb"
    Tool @('--led-restore', '--yes')
    if (-not $DryRun) { Start-Sleep -Seconds 3 }
    Read-Host '  The button should be steady cyan now. Press Enter, then watch it' | Out-Null
    $lt = @()
    foreach ($p in $pages) { $lt += "w00:$p$lo"; $lt += "b03:$(Pattern $rgb)" }
    $lt += 'w00:FD81', 'b03:01,00', 'w00:E080', "c01:$op"
    Tool (@('--lt') + $lt + '--yes')
    $a = Read-Host "  $what (y/n, then anything you noticed)"
    Log "  you said: $a"
    return $a.Trim().ToLower().StartsWith('y')
}

$all = 'F0', 'F1', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'FA', 'FB', 'FE', 'FF'
$question = 'Did the button show a colour other than cyan (steady or breathing), or the keyboard change?'

# ---- Phase 1: find an opcode/low byte that reacts at all
$hit = $null
:outer foreach ($lo in '80', '81') {
    foreach ($op in 'A0', 'A1', 'C2') {
        if (Round $all $lo $op 'FF0000' $question) { $hit = @{ lo = $lo; op = $op }; break outer }
    }
}
if (-not $hit) {
    Log "`nRESULT: no reaction from any page/opcode combination."
    Tool @('--led-restore', '--yes')
    exit 0
}
Log "`nPhase 1 hit: low byte $($hit.lo), opcode $($hit.op)"

# ---- Phase 2: halve the page list
$cand = $all
while ($cand.Count -gt 1) {
    $n = [Math]::Ceiling($cand.Count / 2)
    $a = $cand[0..($n - 1)]
    $b = $cand[$n..($cand.Count - 1)]
    if (Round $a $hit.lo $hit.op 'FF0000' $question) { $cand = $a; continue }
    if (Round $b $hit.lo $hit.op 'FF0000' $question) { $cand = $b; continue }
    Log "`nRESULT: neither half alone reacts - needs pages from both: [$($a -join ' ')] + [$($b -join ' ')]"
    Tool @('--led-restore', '--yes')
    exit 0
}
$page = $cand[0]
Log "`nPhase 2: page $page"

# ---- Phase 3: confirm with other colours (also shows the byte order)
foreach ($c in @(@('00FF00', 'GREEN'), @('0000FF', 'BLUE'))) {
    Round @($page) $hit.lo $hit.op $c[0] "Sent $($c[1]) to page $page. What colour did the button turn? (y if it changed, then name the colour)" | Out-Null
}
Log "`nRESULT: page $page, low byte $($hit.lo), opcode $($hit.op) - see the colour answers above"
Tool @('--led-restore', '--yes')
