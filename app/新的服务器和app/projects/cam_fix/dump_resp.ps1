# -*- coding: utf-8 -*-
$b = [IO.File]::ReadAllText('d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky', (New-Object System.Text.UTF8Encoding($false)))
# -*- coding: utf-8 -*-
$b = [IO.File]::ReadAllText('d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky', (New-Object System.Text.UTF8Encoding($false)))
$names = @{}
foreach ($m in [regex]::Matches($b, 'component_type="Web"[^>]*is_generic="false" instance_name="([^"]+)"')) { $names[$m.Groups[1].Value] = $true }
Write-Output "Web instances: $($names.Keys -join ', ')"
$i = $b.IndexOf('event_name="GotText"')
$start = $b.LastIndexOf('<block type="component_event"', $i)
# find enclosing block end: crude - take 6000 chars, then trim to balanced-ish by locating 'event_name' of NEXT component_event
$chunk = $b.Substring($start, [Math]::Min(7000, $b.Length - $start))
$next = $chunk.IndexOf('component_event', 200)
if ($next -gt 0) { $chunk = $chunk.Substring(0, $next - 60) }
Write-Output "=== GotText handler (len $($chunk.Length)) ==="
Write-Output $chunk



