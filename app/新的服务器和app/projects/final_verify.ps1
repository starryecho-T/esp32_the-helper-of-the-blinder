$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$verifyDir = 'd:\projects\verify_extract'
if (Test-Path $verifyDir) { Remove-Item -Recurse -Force $verifyDir }
[System.IO.Compression.ZipFile]::ExtractToDirectory('d:\projects\cane_controller_merged.aia', $verifyDir)

# 1. verify BKY: parse XML and check block structure
$raw = [System.IO.File]::ReadAllText($verifyDir + '\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky', [System.Text.Encoding]::UTF8)
[xml]$doc = $raw
$nsm = New-Object System.Xml.XmlNamespaceManager($doc.NameTable)
$nsm.AddNamespace('ns', 'https://developers.google.com/blockly/xml')
# find the AfterPicking event of 导入交通图片
$events = $doc.SelectNodes('//ns:block[@type="component_event"]/ns:mutation[@instance_name="导入交通图片"][@event_name="AfterPicking"]', $nsm)
Write-Output ('AfterPicking events found: ' + $events.Count)
$evtBlock = $events[0].ParentNode
# walk the DO statement chain and count blocks + collect method calls
$chain = New-Object System.Collections.ArrayList
$n = $evtBlock.SelectSingleNode('ns:statement[@name="DO"]/ns:block', $nsm)
while ($null -ne $n) {
  $mut = $n.SelectSingleNode('ns:mutation', $nsm)
  $desc = $n.GetAttribute('type')
  if ($mut) {
    $inst = $mut.GetAttribute('instance_name'); $pn = $mut.GetAttribute('property_name'); $mn = $mut.GetAttribute('method_name')
    if ($pn) { $desc += (':' + $inst + '.' + $pn) }
    if ($mn) { $desc += (':' + $inst + '.' + $mn + '()') }
  }
  $txt = $n.SelectSingleNode('.//ns:field[@name="TEXT"]', $nsm)
  if ($txt) { $desc += ' text="' + $txt.InnerText + '"' }
  [void]$chain.Add($desc)
  $n = $n.SelectSingleNode('ns:next/ns:block', $nsm)
}
Write-Output '--- AfterPicking statement chain:'
$chain | ForEach-Object { Write-Output ('  ' + $_) }

# verify button Click event still intact
$btn = $doc.SelectNodes('//ns:block[@type="component_event"]/ns:mutation[@instance_name="开始交通灯识别"][@event_name="Click"]', $nsm)
Write-Output ('Button Click events found: ' + $btn.Count)
Write-Output ('PostFile total occurrences: ' + ([regex]::Matches($raw, 'PostFile')).Count)

# 2. verify SCM texts
$scm = [System.IO.File]::ReadAllText($verifyDir + '\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.scm', [System.Text.Encoding]::UTF8)
$jstart = $scm.IndexOf('{'); $jend = $scm.LastIndexOf('}')
$obj = ($scm.Substring($jstart, $jend - $jstart + 1)) | ConvertFrom-Json
function Find-Comp($comps, $name) {
  foreach ($c in $comps) {
    if ($c.'$Name' -eq $name) { return $c }
    if ($c.PSObject.Properties['$Components']) { $r = Find-Comp $c.'$Components' $name; if ($r) { return $r } }
  }
  return $null
}
$all = $obj.Properties.'$Components'
$ip = Find-Comp $all '导入交通图片'
$lb = Find-Comp $all '交通灯识别说明'
Write-Output ('ImagePicker Text: ' + $ip.Text)
Write-Output ('Instruction Text: ' + $lb.Text)