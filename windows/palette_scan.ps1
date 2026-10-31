<#
  palette_scan.ps1 - map the Mode-button preset colours above code 20.

  Each code is applied with --led-raw (the normal 0xCD preset, so each one flashes).
  Known: 0 off, 1 green, 2 white, 3 cyan, 4 purple, 5 pink, 6 orange, 7 red,
  8 dim cyan, 9 dim white, 10-20 white getting brighter. Yellow and blue were seen
  somewhere above 20 on Linux.

  Run from the project folder (no Administrator needed):
    powershell -ExecutionPolicy Bypass -File windows\palette_scan.ps1            # codes 21..255
    powershell -ExecutionPolicy Bypass -File windows\palette_scan.ps1 -From 80   # resume
  At each prompt type the colour you see (e.g. "yellow", "dim blue", "off").
    Enter = same as the previous code    q = stop (resume later with -From)
  Results are appended to windows\palette_scan.log as "code,colour".
#>
param([int]$From = 21, [int]$To = 255)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe  = Join-Path $root 'predator_i2ec.exe'
$log  = Join-Path $PSScriptRoot 'palette_scan.log'
Add-Content $log "# palette_scan $(Get-Date -Format s) codes $From..$To"

$last = ''
for ($c = $From; $c -le $To; $c++) {
    $out = & $exe --led-raw $c --yes 2>&1
    if ($out -match 'FAIL') { $out | ForEach-Object { Write-Host "    $_" }; Add-Content $log "# code $c FAILED - stopped"; break }
    Start-Sleep -Milliseconds 1500    # let the flash finish
    $a = Read-Host ("code {0,3} (0x{0:X2})  colour? [Enter = {1}]" -f $c, $(if ($last) { $last } else { 'same' }))
    if ($a -eq 'q') { Add-Content $log "# stopped before code $c (resume with -From $c)"; break }
    if ($a) { $last = $a.Trim() }
    Add-Content $log "$c,$last"
}

& $exe --led-restore --yes | Out-Null
Write-Host "Restored the stock colour. Log: $log"
