<#
  PredatorLights.ps1 - Acer Predator PHN16-71: thermal mode + keyboard RGB via BIOS WMI
  (the same WMI methods Linux acer-wmi / PredatorSense use; no driver needed).

  Run in an *elevated* PowerShell:
    Set-ExecutionPolicy -Scope Process Bypass
    .\PredatorLights.ps1 info                                # list the gaming WMI methods
    .\PredatorLights.ps1 getmode
    .\PredatorLights.ps1 mode turbo                          # eco|quiet|balanced|performance|turbo
    .\PredatorLights.ps1 kbd ff0000 00ff00 0000ff ffffff 100 # 4 zones + brightness 0-100
    .\PredatorLights.ps1 kbd ff8800 60                       # one colour, all zones
    .\PredatorLights.ps1 effect breathing 5 ff0000 [100]     # breathing|neon|wave|shifting|zoom, speed 1-9

  Method IDs (from the PHN16-71 ACPI, GUID 7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56):
    6  set zone colour   [mask, R, G, B]            masks 1,2,4,8 (0x0F = all)
    7  get zone colour   [mask] -> [status, R, G, B]
    20 set effect        [mode, speed, bright, 0, dir, R, G, B, 0, 1, 0...] (16 bytes)
    21 get effect        [1] -> [status, mode, speed, bright, 0, dir, R, G, B, ...]
    22 set misc setting  [0x0B, profile]  profile: quiet 0, balanced 1, perf 4, turbo 5, eco 6
    23 get misc setting  [0x0B]

  The Mode-button LED follows the thermal mode. Its other preset colours and effects
  need direct EC port access: use ..\predator_i2ec.exe (README, "Windows").

  NOTE: written on Linux from the decoded ACPI tables; not yet run on Windows.
  Run "info" first and check that the method list matches the IDs above.
#>
param([Parameter(Position = 0)][string]$Command = 'info',
      [Parameter(Position = 1, ValueFromRemainingArguments)][string[]]$Rest)

$ErrorActionPreference = 'Stop'
$Guid = '7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56'

function Find-GamingClass {
    foreach ($c in Get-CimClass -Namespace root/wmi) {
        $q = $c.CimClassQualifiers['guid']
        if ($q -and ($q.Value -replace '[{}]', '') -ieq $Guid) { return $c }
    }
    throw "No WMI class with GUID $Guid found (not an Acer gaming laptop, or not elevated?)"
}

$Class = Find-GamingClass
$Instance = Get-CimInstance -Namespace root/wmi -ClassName $Class.CimClassName | Select-Object -First 1
if (-not $Instance) { throw "WMI class $($Class.CimClassName) has no instance" }

function Get-Method([int]$Id) {
    foreach ($m in $Class.CimClassMethods) {
        $q = $m.Qualifiers['WmiMethodId']
        if ($q -and [int]$q.Value -eq $Id) { return $m }
    }
    throw "WMI method id $Id not found in $($Class.CimClassName)"
}

# Call a method by ID with a byte payload, converting to whatever the MOF parameter type is.
function Invoke-Gaming([int]$Id, [byte[]]$Bytes) {
    $m = Get-Method $Id
    $in = $m.Parameters | Where-Object { $_.Qualifiers['In'] } | Select-Object -First 1
    if (-not $in) { throw "method $($m.Name) has no input parameter" }
    $t = $in.CimType.ToString()
    switch -Wildcard ($t) {
        '*Array' {
            $size = $in.Qualifiers['Max']
            $n = if ($size) { [int]$size.Value } else { [Math]::Max($Bytes.Length, 16) }
            $buf = New-Object byte[] $n
            [Array]::Copy($Bytes, $buf, [Math]::Min($Bytes.Length, $n))
            $val = $buf
        }
        'UInt64' { $b = New-Object byte[] 8; [Array]::Copy($Bytes, $b, [Math]::Min($Bytes.Length, 8)); $val = [BitConverter]::ToUInt64($b, 0) }
        'UInt32' { $b = New-Object byte[] 4; [Array]::Copy($Bytes, $b, [Math]::Min($Bytes.Length, 4)); $val = [BitConverter]::ToUInt32($b, 0) }
        default  { throw "unsupported parameter type $t on $($m.Name)" }
    }
    $r = Invoke-CimMethod -InputObject $Instance -MethodName $m.Name -Arguments @{ $in.Name = $val }
    # Invoke-CimMethod returns the out parameters as plain properties
    $out = $r.PSObject.Properties['gmOutput']
    if (-not $out) { return @() }
    $v = $out.Value
    if ($v -is [array]) { return [byte[]]$v }
    return [BitConverter]::GetBytes([UInt64]$v)
}

function Hex([byte[]]$b) { ($b | ForEach-Object { '{0:X2}' -f $_ }) -join ' ' }
function Rgb([string]$s) {
    $s = $s.TrimStart('#')
    if ($s -notmatch '^[0-9a-fA-F]{6}$') { throw "bad colour '$s' (use RRGGBB)" }
    return [byte[]]@([Convert]::ToByte($s.Substring(0, 2), 16), [Convert]::ToByte($s.Substring(2, 2), 16), [Convert]::ToByte($s.Substring(4, 2), 16))
}

$Profiles = [ordered]@{ quiet = 0; balanced = 1; performance = 4; turbo = 5; eco = 6 }
$Effects  = @{ breathing = 1; neon = 2; wave = 3; shifting = 4; zoom = 5 }

switch ($Command.ToLower()) {
    'info' {
        "WMI class: $($Class.CimClassName)"
        foreach ($m in $Class.CimClassMethods | Sort-Object { [int]$_.Qualifiers['WmiMethodId'].Value }) {
            $id = $m.Qualifiers['WmiMethodId'].Value
            $ps = ($m.Parameters | ForEach-Object { "$(if ($_.Qualifiers['In']) {'in'} else {'out'}) $($_.CimType) $($_.Name)" }) -join ', '
            '{0,3}  {1,-40} {2}' -f $id, $m.Name, $ps
        }
    }
    'getmode' {
        $o = Invoke-Gaming 23 ([byte[]]@(0x0B))
        "raw: $(Hex $o)"
        "(interpretation of the raw bytes is unverified on Windows; compare after 'mode' changes)"
    }
    'mode' {
        $name = $Rest[0]
        if (-not $Profiles.Contains($name)) { throw "mode must be one of: $($Profiles.Keys -join ', ')" }
        $o = Invoke-Gaming 22 ([byte[]]@(0x0B, $Profiles[$name]))
        "set $name -> returned $(Hex $o)  (first byte 00 = OK; turbo usually needs AC power)"
    }
    'kbd' {
        $cols = @(); $bright = 100
        foreach ($a in $Rest) { if ($a -match '^#?[0-9a-fA-F]{6}$') { $cols += , (Rgb $a) } else { $bright = [int]$a } }
        if ($cols.Count -lt 1 -or $bright -lt 0 -or $bright -gt 100) { throw "usage: kbd RRGGBB [RRGGBB x3] [0-100]" }
        while ($cols.Count -lt 4) { $cols += , $cols[-1] }
        $masks = 1, 2, 4, 8
        for ($z = 0; $z -lt 4; $z++) {
            $o = Invoke-Gaming 6 ([byte[]]@($masks[$z]) + $cols[$z])
            "zone $($z + 1): $(Hex $cols[$z]) -> $(Hex $o)"
        }
        # static effect makes the BIOS push the stored zone colours to the lighting chip
        $c = $cols[0]
        $o = Invoke-Gaming 20 ([byte[]]@(0, 0, $bright, 0, 1, $c[0], $c[1], $c[2], 0, 1, 0, 0, 0, 0, 0, 0))
        "apply static, brightness $bright -> $(Hex $o)"
    }
    'effect' {
        $e = $Rest[0]; $speed = [int]$Rest[1]; $c = Rgb $Rest[2]
        $bright = if ($Rest.Count -ge 4) { [int]$Rest[3] } else { 100 }
        if (-not $Effects.ContainsKey($e)) { throw "effect must be one of: $($Effects.Keys -join ', ')" }
        if ($speed -lt 1 -or $speed -gt 9) { throw "speed 1-9" }
        $o = Invoke-Gaming 20 ([byte[]]@($Effects[$e], $speed, $bright, 0, 1, $c[0], $c[1], $c[2], 0, 1, 0, 0, 0, 0, 0, 0))
        "$e speed $speed colour $(Hex $c) -> $(Hex $o)"
    }
    default { throw "unknown command '$Command' (info|getmode|mode|kbd|effect)" }
}
