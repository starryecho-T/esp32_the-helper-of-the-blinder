# -*- coding: utf-8 -*-
$p = 'd:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.scm'
$t = [IO.File]::ReadAllText($p, (New-Object System.Text.UTF8Encoding($false)))
$lines = $t -split "`n"
Write-Output ("lines=" + $lines.Count)
$o = $lines[2].TrimEnd("`r") | ConvertFrom-Json
Write-Output ("root props: " + (($o.PSObject.Properties | ForEach-Object Name) -join ','))
function Walk($obj, $depth) {
    $n = $obj.'$Name'; $ty = $obj.'$Type'
    Write-Output (('  ' * $depth) + $n + ' [' + $ty + ']')
    if ($obj.PSObject.Properties.Match('$Components').Count -gt 0) {
        foreach ($c in $obj.'$Components') { Walk $c ($depth + 1) }
    }
}
Walk $o 0
$target = [regex]::Unescape('\u4ea4\u901a\u706f\u56fe\u7247\u64cd\u4f5c\u884c')
Write-Output ("target length: " + $target.Length + " chars: " + (($target.ToCharArray() | ForEach-Object { '{0:X4}' -f [int]$_ }) -join ' '))
