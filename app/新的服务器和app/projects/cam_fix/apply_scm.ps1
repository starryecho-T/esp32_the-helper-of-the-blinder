# -*- coding: utf-8 -*-
# Part 1: Screen1.scm edits (pure ASCII script)
$ErrorActionPreference = 'Stop'
$dir = 'd:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg'
$scmPath = Join-Path $dir 'Screen1.scm'
$utf8nb = New-Object System.Text.UTF8Encoding($false)
$scm = [IO.File]::ReadAllText($scmPath, $utf8nb)

# 1) insert new button after the Start-Recognition button (uuid -2100000003, unique)
$anchorScm = ',"Uuid":"-2100000003"}]}'
if (([regex]::Matches($scm, [regex]::Escape($anchorScm))).Count -ne 1) { throw "SCM anchor not unique: $anchorScm" }
$newBtn = '{"$Name":"\u8bc6\u522b\u6444\u50cf\u5934\u753b\u9762","$Type":"Button","$Version":"8","BackgroundColor":"&HFF1565C0","Height":"55","Width":"-2","Text":"\u6444\u50cf\u5934\u8bc6\u522b","TextColor":"&HFFFFFFFF","Uuid":"-2100000020"}'
if ($scm.Contains('-2100000020')) { throw 'uuid -2100000020 already used' }
$scm2 = $scm.Replace($anchorScm, (',"Uuid":"-2100000003"},' + $newBtn + ']}'))

# 2) update explanatory label text
$oldExp = '\u70b9\u51fb\u201c\u5bfc\u5165\u5e76\u8bc6\u522b\u201d\u9009\u62e9\u9053\u8def\u56fe\u7247\uff0c\u7cfb\u7edf\u5c06\u81ea\u52a8\u8bc6\u522b\u5e76\u8bed\u97f3\u64ad\u62a5\u7ed3\u679c\u3002'
$newExp = '\u70b9\u51fb\u201c\u6444\u50cf\u5934\u8bc6\u522b\u201d\u76f4\u63a5\u8bc6\u522b\u5934\u6234\u6444\u50cf\u5934\u753b\u9762\uff1b\u4e5f\u53ef\u70b9\u51fb\u201c\u5bfc\u5165\u5e76\u8bc6\u522b\u201d\u4ece\u76f8\u518c\u9009\u56fe\u8bc6\u522b\u3002\u7ed3\u679c\u81ea\u52a8\u8bed\u97f3\u64ad\u62a5\u3002'
if (([regex]::Matches($scm2, [regex]::Escape($oldExp))).Count -ne 1) { throw 'SCM explain-label anchor not unique' }
$scm2 = $scm2.Replace($oldExp, $newExp)

# validate JSON body: scm format = line0 '#|', line1 '$JSON', line2 JSON, line3 '|#'
$lines = $scm2 -split "`n"
if ($lines.Count -ne 4 -or $lines[0].Trim() -ne '#|' -or $lines[1].Trim() -ne '$JSON' -or $lines[3].Trim() -ne '|#') { throw "unexpected scm layout: $($lines.Count) lines" }
try { $scmObj = $lines[2].TrimEnd("`r") | ConvertFrom-Json } catch { throw "SCM JSON invalid: $($_.Exception.Message)" }
function Find-Comp($obj, [string]$name) {
    if ($obj.'$Name' -eq $name) { return $obj }
    if ($obj.PSObject.Properties['$Components']) {
        foreach ($c in $obj.'$Components') { $r = Find-Comp $c $name; if ($r) { return $r } }
    }
    return $null
}
$row = Find-Comp $scmObj.Properties ([regex]::Unescape('\u4ea4\u901a\u706f\u56fe\u7247\u64cd\u4f5c\u884c'))  # traffic ops row
if (-not $row) { throw 'ops row not found' }
Write-Output ("row children: " + $row.'$Components'.Count + " -> " + (($row.'$Components' | ForEach-Object { $_.'$Name' }) -join ' | '))
$btn = Find-Comp $scmObj.Properties ([regex]::Unescape('\u8bc6\u522b\u6444\u50cf\u5934\u753b\u9762'))  # new button
if (-not $btn -or $btn.'$Type' -ne 'Button') { throw 'new button missing after edit' }
if (([regex]::Matches($scm2, '-2100000020')).Count -ne 1) { throw 'new uuid count != 1' }
[IO.File]::WriteAllText($scmPath, $scm2, $utf8nb)
$bytes = [IO.File]::ReadAllBytes($scmPath)
if ($bytes[0] -eq 0xEF) { throw 'BOM appeared!' }
Write-Output ("SCM OK: old=$($scm.Length) new=$($scm2.Length)")
