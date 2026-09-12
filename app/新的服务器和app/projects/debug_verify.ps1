$ErrorActionPreference = 'Stop'
$verifyDir = 'd:\projects\verify_extract'
$bkyFile = Join-Path $verifyDir 'src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky'
$raw = [System.IO.File]::ReadAllText($bkyFile, [System.Text.Encoding]::UTF8)
[xml]$doc = $raw
$nsm = New-Object System.Xml.XmlNamespaceManager($doc.NameTable)
$nsm.AddNamespace('ns', 'https://developers.google.com/blockly/xml')
$muts = $doc.SelectNodes('//ns:mutation[@event_name]', $nsm)
Write-Output ('total event mutations: ' + $muts.Count)
foreach ($m in $muts) {
  $inst = $m.GetAttribute('instance_name')
  $evt = $m.GetAttribute('event_name')
  $mark = ''
  if ($evt -eq 'AfterPicking' -and $inst -eq ([char]0x5BFC + [string][char]0x5165 + [string][char]0x4EA4 + [string][char]0x901A + [string][char]0x56FE + [string][char]0x7247)) { $mark = '  <== TARGET' }
  Write-Output ('  ' + $inst + ' . ' + $evt + $mark)
}