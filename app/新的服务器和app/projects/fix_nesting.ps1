$ErrorActionPreference = 'Stop'
$bkyPath = "d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$bky = [System.IO.File]::ReadAllText($bkyPath, [System.Text.Encoding]::UTF8)

# Fix A: move <next> INSIDE the confidence block (before its </block>)
$oldA = '</value></block><next><block type="component_set_get" id="mrg0seturl0x0001">'
$newA = '</value><next><block type="component_set_get" id="mrg0seturl0x0001">'
$cA = ([regex]::Matches($bky, [regex]::Escape($oldA))).Count
if ($cA -ne 1) { throw "fixA count = $cA" }
$bky = $bky.Replace($oldA, $newA)

# Fix B: re-close confidence block after the inserted chain
$oldB = 'global selectedTrafficImage</field></block></value></block></next></block></next></next></block>'
$newB = 'global selectedTrafficImage</field></block></value></block></next></block></next></block></next></block>'
$cB = ([regex]::Matches($bky, [regex]::Escape($oldB))).Count
if ($cB -ne 1) { throw "fixB count = $cB" }
$bky = $bky.Replace($oldB, $newB)

[xml]$null = $bky
[System.IO.File]::WriteAllText($bkyPath, $bky, $utf8NoBom)
Write-Output ('BKY nesting fixed, length: ' + $bky.Length)