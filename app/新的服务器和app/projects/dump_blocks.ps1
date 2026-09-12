$raw = [System.IO.File]::ReadAllText("d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky", [System.Text.Encoding]::UTF8)
[xml]$doc = $raw
$nsm = New-Object System.Xml.XmlNamespaceManager($doc.NameTable)
$nsm.AddNamespace('ns', 'https://developers.google.com/blockly/xml')
$topBlocks = $doc.SelectNodes('/ns:xml/ns:block', $nsm)
$i = 0
foreach ($b in $topBlocks) {
  $i++
  $m = $b.SelectSingleNode('ns:mutation', $nsm)
  $tag = ''
  if ($m) { $tag = $m.GetAttribute('instance_name') + '.' + $m.GetAttribute('event_name') }
  [void]$b.OuterXml
  $safe = ($tag -replace '[^A-Za-z0-9_.-]', '_')
  if ($safe -eq '') { $safe = $b.GetAttribute('type') }
  $file = ('d:\projects\blocks\block_{0:D2}_{1}.xml' -f $i, $safe)
  [System.IO.File]::WriteAllText($file, $b.OuterXml, (New-Object System.Text.UTF8Encoding($false)))
}
Write-Output ('Dumped ' + $i + ' blocks')