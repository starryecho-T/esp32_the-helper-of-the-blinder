# -*- coding: utf-8 -*-
$p = 'd:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.scm'
$t = [IO.File]::ReadAllText($p, (New-Object System.Text.UTF8Encoding($false)))
Write-Output "len=$($t.Length)"
Write-Output ("first 80 chars: " + $t.Substring(0, 80).Replace("`n", '\n').Replace("`r", '\r'))
$bi = [IO.File]::ReadAllBytes($p)
Write-Output ("first bytes: " + (($bi[0..3] | ForEach-Object { $_.ToString('X2') }) -join ' '))
$lines = $t -split "`n"
Write-Output ("line count: " + $lines.Count)
for ($k = 0; $k -lt [Math]::Min(5, $lines.Count); $k++) { Write-Output ("line${k}: [" + $lines[$k].TrimEnd("`r") + "] len=" + $lines[$k].Length) }
