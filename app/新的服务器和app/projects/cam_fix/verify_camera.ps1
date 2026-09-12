# -*- coding: utf-8 -*-
# Final verification of cane_controller_camera_detect.aia (pure ASCII script)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$aia = 'd:\projects\cane_controller_camera_detect.aia'
$orig = 'd:\projects\cane_controller_2__v2hfg.aia'
$utf8nb = New-Object System.Text.UTF8Encoding($false)
$U = { param($s) [regex]::Unescape($s) }
$pass = 0; $fail = 0
function Check($cond, $label) {
    if ($cond) { Write-Output ("[PASS] " + $label); $script:pass++ }
    else { Write-Output ("[FAIL] " + $label); $script:fail++ }
}

# open both zips, compare entry sets
$zin1 = [System.IO.Compression.ZipFile]::OpenRead($orig)
$zin2 = [System.IO.Compression.ZipFile]::OpenRead($aia)
$names1 = $zin1.Entries.FullName | Sort-Object
$names2 = $zin2.Entries.FullName | Sort-Object
Check (($names1 -join ';') -eq ($names2 -join ';')) ('entry set identical to original (' + $names2.Count + ' entries)')
$zin1.Dispose()

# read the two entries from the new zip
function ReadEntry($zip, $name) {
    $e = $zip.GetEntry($name)
    if (-not $e) { throw "entry missing: $name" }
    $sr = New-Object System.IO.StreamReader($e.Open(), $utf8nb)
    $t = $sr.ReadToEnd(); $sr.Dispose(); return $t
}
$prefix = 'src/appinventor/ai_zjj44450/cane_controller_2__v2hfg/'
$bky = ReadEntry $zin2 ($prefix + 'Screen1.bky')
$scm = ReadEntry $zin2 ($prefix + 'Screen1.scm')
$zin2.Dispose()

# ---------- SCM checks ----------
$lines = $scm -split "`n"
$obj = $lines[2].TrimEnd("`r") | ConvertFrom-Json
function Find-Comp($obj, [string]$name) {
    if ($obj.'$Name' -eq $name) { return $obj }
    if ($obj.PSObject.Properties.Match('$Components').Count -gt 0) {
        foreach ($c in $obj.'$Components') { $r = Find-Comp $c $name; if ($r) { return $r } }
    }
    return $null
}
$props = $obj.Properties
$row = Find-Comp $props ([regex]::Unescape('\u4ea4\u901a\u706f\u56fe\u7247\u64cd\u4f5c\u884c'))
Check ($row -and $row.'$Components'.Count -eq 3) 'scm: ops row has 3 children'
$btn = Find-Comp $props ([regex]::Unescape('\u8bc6\u522b\u6444\u50cf\u5934\u753b\u9762'))
Check ($btn -and $btn.'$Type' -eq 'Button' -and $btn.Text -eq ([regex]::Unescape('\u6444\u50cf\u5934\u8bc6\u522b'))) 'scm: new button with correct text'
$uuids = @{}
function Collect-Uuid($o) {
    if ($o.PSObject.Properties.Match('Uuid').Count -gt 0) { $script:uuids[$o.Uuid] = $script:uuids[$o.Uuid] + 1 }
    if ($o.PSObject.Properties.Match('$Components').Count -gt 0) { foreach ($c in $o.'$Components') { Collect-Uuid $c } }
}
Collect-Uuid $props
$dup = $uuids.Keys | Where-Object { $uuids[$_] -gt 1 }
Check (-not $dup) ('scm: all Uuids unique (dup: ' + ($dup -join ',') + ')')
$exp = Find-Comp $props ([regex]::Unescape('\u4ea4\u901a\u706f\u8bc6\u522b\u8bf4\u660e'))
$expectedExp = [regex]::Unescape('\u70b9\u51fb\u201c\u6444\u50cf\u5934\u8bc6\u522b\u201d\u76f4\u63a5\u8bc6\u522b\u5934\u6234\u6444\u50cf\u5934\u753b\u9762\uff1b\u4e5f\u53ef\u70b9\u51fb\u201c\u5bfc\u5165\u5e76\u8bc6\u522b\u201d\u4ece\u76f8\u518c\u9009\u56fe\u8bc6\u522b\u3002\u7ed3\u679c\u81ea\u52a8\u8bed\u97f3\u64ad\u62a5\u3002')
Check ($exp.Text -eq $expectedExp) 'scm: explanatory label updated'
$sb = [System.Text.Encoding]::UTF8.GetBytes($scm)
Check ($sb[0] -ne 0xEF) 'scm: no BOM'

# ---------- BKY checks ----------
$xml = New-Object System.Xml.XmlDocument
$xml.PreserveWhitespace = $true
$xml.LoadXml($bky)
$nsm = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$nsm.AddNamespace('x', 'https://developers.google.com/blockly/xml')
$nsm.AddNamespace('h', 'http://www.w3.org/1999/xhtml')
$top = $xml.SelectNodes('/x:xml/x:block', $nsm)
Check ($top.Count -eq 29) ('bky: 29 top-level blocks (got ' + $top.Count + ')')
# new camera Click event
$nameBtn = [regex]::Unescape('\u8bc6\u522b\u6444\u50cf\u5934\u753b\u9762')
$newEvt = $null
foreach ($blk in $top) {
    $mut = $blk.SelectSingleNode('h:mutation', $nsm)
    if ($mut -and $mut.GetAttribute('event_name') -eq 'Click' -and $mut.GetAttribute('instance_name') -eq $nameBtn) { $newEvt = $blk; break }
}
Check ($null -ne $newEvt) 'bky: camera button Click event exists'
# chain signature
$sig = @()
$n = $newEvt.SelectSingleNode('x:statement[@name="DO"]/x:block', $nsm)
while ($n) {
    $m = $n.SelectSingleNode('h:mutation', $nsm)
    $sig += ($n.GetAttribute('type') + '/' + $m.GetAttribute('property_name') + $m.GetAttribute('method_name'))
    $n = $n.SelectSingleNode('x:next/x:block', $nsm)
}
Check (($sig -join '>') -eq 'component_set_get/Text>component_set_get/Text>component_set_get/Url>component_method/Get') ('bky: chain = ' + ($sig -join ' > '))
# join pieces
$adds = $newEvt.SelectNodes('.//x:block[@type="text_join"]/x:value', $nsm)
Check ($adds.Count -eq 3) 'bky: text_join has 3 inputs'
$v0 = $newEvt.SelectSingleNode('.//x:block[@type="text_join"]/x:value[@name="ADD0"]/x:block/x:field[@name="VAR"]', $nsm)
$v1 = $newEvt.SelectSingleNode('.//x:block[@type="text_join"]/x:value[@name="ADD1"]/x:block/x:field[@name="TEXT"]', $nsm)
$v2 = $newEvt.SelectSingleNode('.//x:block[@type="text_join"]/x:value[@name="ADD2"]/x:block/x:field[@name="VAR"]', $nsm)
Check ($v0.InnerText -eq 'global TrafficURL' -and $v1.InnerText -eq '?cam=' -and $v2.InnerText -eq 'global CameraURL') 'bky: Url = TrafficURL + ?cam= + CameraURL'
# existing flows intact
Check (([regex]::Matches($bky, 'method_name="PostFile"')).Count -eq 2) 'bky: both PostFile flows still present'
$got = $xml.SelectNodes('//x:block[@type="component_event"]/h:mutation[@event_name="GotText"]', $nsm)
Check ($got.Count -eq 1) 'bky: GotText handler still exactly 1'
$dupIds = $xml.SelectNodes('//*[@id]') | Group-Object { $_.GetAttribute('id') } | Where-Object { $_.Count -gt 1 }
Check (-not $dupIds) 'bky: no duplicate block ids'
$bb = [System.Text.Encoding]::UTF8.GetBytes($bky)
Check ($bb[0] -ne 0xEF) 'bky: no BOM'

Write-Output ''
Write-Output ("RESULT: " + $pass + " passed, " + $fail + " failed")
if ($fail -gt 0) { exit 1 }
