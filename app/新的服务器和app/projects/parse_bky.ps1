$raw = [System.IO.File]::ReadAllText("d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky", [System.Text.Encoding]::UTF8)
[xml]$doc = $raw
$nsm = New-Object System.Xml.XmlNamespaceManager($doc.NameTable)
$nsm.AddNamespace('ns', 'https://developers.google.com/blockly/xml')
$sb = New-Object System.Text.StringBuilder
function Show-Node($node, $indent) {
  if ($null -eq $node) { return }
  if ($node -is [System.Xml.XmlElement]) {
    if ($node.LocalName -eq 'block') {
      $info = $node.GetAttribute('type')
      $m = $node.SelectSingleNode('ns:mutation', $script:nsm)
      if ($m) {
        $evt = $m.GetAttribute('event_name'); $inst = $m.GetAttribute('instance_name'); $meth = $m.GetAttribute('method_name'); $pn = $m.GetAttribute('property_name'); $so = $m.GetAttribute('set_or_get')
        if ($evt) { $info += " [$inst.$evt]" }
        if ($meth) { $info += " [$inst.$meth]" }
        if ($pn) { $info += " [$so $inst.$pn]" }
      }
      $procName = $node.SelectSingleNode("ns:field[@name='NAME']", $script:nsm)
      if ($procName -and $node.GetAttribute('type') -like 'procedures_*') { $info += ' name=' + $procName.InnerText }
      foreach ($f in $node.SelectNodes('ns:field', $script:nsm)) { $info += ' |' + $f.GetAttribute('name') + '=' + $f.InnerText }
      [void]$script:sb.AppendLine($indent + $info)
      foreach ($v in $node.SelectNodes('ns:value', $script:nsm)) {
        [void]$script:sb.AppendLine($indent + '  arg<' + $v.GetAttribute('name') + '>:')
        Show-Node $v.SelectSingleNode('ns:block', $script:nsm) ($indent + '    ')
      }
      foreach ($st in $node.SelectNodes('ns:statement', $script:nsm)) {
        [void]$script:sb.AppendLine($indent + '  do<' + $st.GetAttribute('name') + '>:')
        Show-Node $st.SelectSingleNode('ns:block', $script:nsm) ($indent + '    ')
      }
      foreach ($n in $node.SelectNodes('ns:next', $script:nsm)) {
        [void]$script:sb.AppendLine($indent + '  >>>')
        Show-Node $n.SelectSingleNode('ns:block', $script:nsm) ($indent + '  ')
      }
    }
  }
}
$topBlocks = $doc.SelectNodes('/ns:xml/ns:block', $nsm)
$i = 0
foreach ($b in $topBlocks) {
  $i++
  [void]$sb.AppendLine('=== TOP-LEVEL BLOCK #' + $i + ' ===')
  Show-Node $b ''
}
[System.IO.File]::WriteAllText('d:\projects\bky_outline.txt', $sb.ToString(), [System.Text.Encoding]::UTF8)
Write-Output ('Total top blocks: ' + $i + '; outline chars: ' + $sb.Length)