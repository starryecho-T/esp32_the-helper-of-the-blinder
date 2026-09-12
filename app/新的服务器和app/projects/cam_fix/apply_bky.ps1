# -*- coding: utf-8 -*-
# Part 2: Screen1.bky edit - add camera button Click event (pure ASCII script)
$ErrorActionPreference = 'Stop'
$dir = 'd:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg'
$bkyPath = Join-Path $dir 'Screen1.bky'
$utf8nb = New-Object System.Text.UTF8Encoding($false)
$bky = [IO.File]::ReadAllText($bkyPath, $utf8nb)
$U = { param($s) [regex]::Unescape($s) }

$nameBtn = & $U '\u8bc6\u522b\u6444\u50cf\u5934\u753b\u9762'    # button name
$nameRes = & $U '\u4ea4\u901a\u706f\u8bc6\u522b\u7ed3\u679c'   # result label
$nameCnf = & $U '\u4ea4\u901a\u706f\u8bc6\u522b\u7f6e\u4fe1\u5ea6'  # confidence label
$nameWeb = & $U '\u4ea4\u901a\u706f\u8bc6\u522b\u63a5\u53e3'   # web component
$txtBusy = & $U '\u8bc6\u522b\u4e2d\uff0c\u8bf7\u7a0d\u5019\u2026\u2026'
$txtCnfR = & $U '\u7f6e\u4fe1\u5ea6\uff1a--'

$tpl = @'
<block type="component_event" id="camEvt20260912A" x="3400" y="1226"><mutation xmlns="http://www.w3.org/1999/xhtml" component_type="Button" is_generic="false" instance_name="{0}" event_name="Click"></mutation><field name="COMPONENT_SELECTOR">{0}</field><statement name="DO"><block type="component_set_get" id="camSet20260912B"><mutation xmlns="http://www.w3.org/1999/xhtml" component_type="Label" set_or_get="set" property_name="Text" is_generic="false" instance_name="{1}"></mutation><field name="COMPONENT_SELECTOR">{1}</field><field name="PROP">Text</field><value name="VALUE"><block type="text" id="camTxt20260912C"><field name="TEXT">{4}</field></block></value><next><block type="component_set_get" id="camSet20260912D"><mutation xmlns="http://www.w3.org/1999/xhtml" component_type="Label" set_or_get="set" property_name="Text" is_generic="false" instance_name="{2}"></mutation><field name="COMPONENT_SELECTOR">{2}</field><field name="PROP">Text</field><value name="VALUE"><block type="text" id="camTxt20260912E"><field name="TEXT">{5}</field></block></value><next><block type="component_set_get" id="camSet20260912F"><mutation xmlns="http://www.w3.org/1999/xhtml" component_type="Web" set_or_get="set" property_name="Url" is_generic="false" instance_name="{3}"></mutation><field name="COMPONENT_SELECTOR">{3}</field><field name="PROP">Url</field><value name="VALUE"><block type="text_join" id="camJoin20260912G"><mutation xmlns="http://www.w3.org/1999/xhtml" items="3"></mutation><value name="ADD0"><block type="lexical_variable_get" id="camGet20260912H"><field name="VAR">global TrafficURL</field></block></value><value name="ADD1"><block type="text" id="camTxt20260912I"><field name="TEXT">?cam=</field></block></value><value name="ADD2"><block type="lexical_variable_get" id="camGet20260912J"><field name="VAR">global CameraURL</field></block></value></block></value><next><block type="component_method" id="camMth20260912K"><mutation xmlns="http://www.w3.org/1999/xhtml" component_type="Web" method_name="Get" is_generic="false" instance_name="{3}"></mutation><field name="COMPONENT_SELECTOR">{3}</field></block></next></block></next></block></next></block></statement></block>
'@
$newBlock = $tpl -f $nameBtn, $nameRes, $nameCnf, $nameWeb, $txtBusy, $txtCnfR

foreach ($id in @('camEvt20260912A','camSet20260912B','camTxt20260912C','camSet20260912D','camTxt20260912E','camSet20260912F','camJoin20260912G','camGet20260912H','camTxt20260912I','camGet20260912J','camMth20260912K')) {
    if ($bky.Contains("id=""$id""")) { throw "id collision: $id" }
}
if (([regex]::Matches($bky, '<yacodeblocks')).Count -ne 1) { throw 'yacodeblocks anchor not unique' }
$bky2 = $bky.Replace('<yacodeblocks', $newBlock + '<yacodeblocks')

$xml = New-Object System.Xml.XmlDocument
$xml.PreserveWhitespace = $true
$xml.LoadXml($bky2)
$nsm = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$nsm.AddNamespace('x', 'https://developers.google.com/blockly/xml')
$nsm.AddNamespace('h', 'http://www.w3.org/1999/xhtml')
$top = $xml.SelectNodes('/x:xml/x:block', $nsm)
Write-Output ("top-level blocks: " + $top.Count)
if ($top.Count -ne 29) { throw 'expected 29 top-level blocks' }
$newEvt = $null
foreach ($blk in $top) {
    $mut = $blk.SelectSingleNode('h:mutation', $nsm)
    if ($mut -and $mut.GetAttribute('event_name') -eq 'Click' -and $mut.GetAttribute('instance_name') -eq $nameBtn) { $newEvt = $blk; break }
}
if (-not $newEvt) { throw 'new event not found in parsed XML' }
$do = $newEvt.SelectSingleNode('x:statement[@name="DO"]', $nsm)
$chain = @(); $node = $do.SelectSingleNode('x:block', $nsm)
while ($node -and $node.LocalName -eq 'block') {
    $m = $node.SelectSingleNode('h:mutation', $nsm)
    $chain += ($node.GetAttribute('type') + '/' + $(if ($m) { $m.GetAttribute('property_name') + $m.GetAttribute('method_name') } else { '' }))
    $node = $node.SelectSingleNode('x:next/x:block', $nsm)
}
Write-Output ("DO chain: " + ($chain -join ' -> '))
$join = $newEvt.SelectSingleNode('.//x:block[@type="text_join"]/h:mutation', $nsm)
if (-not $join -or $join.GetAttribute('items') -ne '3') { throw 'text_join items != 3' }
$mid = $newEvt.SelectSingleNode('.//x:block[@type="component_method"]/h:mutation', $nsm)
if (-not $mid -or $mid.GetAttribute('method_name') -ne 'Get') { throw 'Web.Get call missing' }
$dup = $xml.SelectNodes('//*[@id]') | Group-Object { $_.GetAttribute('id') } | Where-Object { $_.Count -gt 1 }
if ($dup) { throw ('duplicate ids: ' + (($dup | ForEach-Object Name) -join ',')) }
[IO.File]::WriteAllText($bkyPath, $bky2, $utf8nb)
$bytes = [IO.File]::ReadAllBytes($bkyPath)
if ($bytes[0] -eq 0xEF) { throw 'BOM appeared!' }
Write-Output ("BKY OK: old=$($bky.Length) new=$($bky2.Length)")
