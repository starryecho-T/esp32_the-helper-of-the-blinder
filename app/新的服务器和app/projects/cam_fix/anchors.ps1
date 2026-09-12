# -*- coding: utf-8 -*-
$ErrorActionPreference = 'Stop'
$scmPath = 'd:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.scm'
$bkyPath = 'd:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky'
$scm = [IO.File]::ReadAllText($scmPath, (New-Object System.Text.UTF8Encoding($false)))
$bky = [IO.File]::ReadAllText($bkyPath, (New-Object System.Text.UTF8Encoding($false)))

Write-Output "=== SCM length: $($scm.Length), BKY length: $($bky.Length) ==="

# 1) area around 开始交通灯识别 button (escaped)
$anchor = '\u5f00\u59cb\u4ea4\u901a\u706f\u8bc6\u522b'
$idx = $scm.IndexOf($anchor)
Write-Output "`n=== SCM around [$anchor] (idx=$idx) ==="
if ($idx -ge 0) { $s=[Math]::Max(0,$idx-80); $len=[Math]::Min(700,$scm.Length-$s); Write-Output $scm.Substring($s,$len) }

# occurrences of the uuid -2100000003
$u = '\-2100000003'
$mc = [regex]::Matches($scm, $u)
Write-Output "`n=== uuid -2100000003 occurrences: $($mc.Count) ==="

# 2) 说明 label current text
$idx2 = $scm.IndexOf('\u8bf4\u660e')
Write-Output "`n=== SCM around 说明 (idx=$idx2) ==="
if ($idx2 -ge 0) { $s=[Math]::Max(0,$idx2-200); $len=[Math]::Min(900,$scm.Length-$s); Write-Output $scm.Substring($s,$len) }

# 3) bky tail
Write-Output "`n=== BKY tail (last 500) ==="
Write-Output $bky.Substring($bky.Length-500)

# 4) URL globals in bky
Write-Output "`n=== TrafficURL / CameraURL occurrences in BKY ==="
foreach ($m in [regex]::Matches($bky, 'http://10\.155\.212\.170[^<"\\]*')) { Write-Output $m.Value }
foreach ($m in [regex]::Matches($bky, 'http://192\.168\.43\.248[^<"\\]*')) { Write-Output $m.Value }

# 5) sample component_set_get dialect
$idx5 = $bky.IndexOf('component_set_get')
Write-Output "`n=== BKY sample component_set_get (idx=$idx5) ==="
if ($idx5 -ge 0) { $s=[Math]::Max(0,$idx5-350); $len=[Math]::Min(900,$bky.Length-$s); Write-Output $bky.Substring($s,$len) }

# 6) sample text_join if any
$idx6 = $bky.IndexOf('text_join')
Write-Output "`n=== BKY sample text_join (idx=$idx6) ==="
if ($idx6 -ge 0) { $s=[Math]::Max(0,$idx6-200); $len=[Math]::Min(700,$bky.Length-$s); Write-Output $bky.Substring($s,$len) }

# 7) sample component_method PostFile + Web event
$idx7 = $bky.IndexOf('PostFile')
Write-Output "`n=== BKY sample PostFile (idx=$idx7) ==="
if ($idx7 -ge 0) { $s=[Math]::Max(0,$idx7-450); $len=[Math]::Min(700,$bky.Length-$s); Write-Output $bky.Substring($s,$len) }

# 8) count top-level blocks & max numeric id length
Write-Output "`n=== <block count: $([regex]::Matches($bky,'<block ').Count) ; yacodeblocks pos: $($bky.IndexOf('<yacodeblocks')) ==="
