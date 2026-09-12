$bkyFile = 'd:\projects\verify_extract\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky'
$raw = [System.IO.File]::ReadAllText($bkyFile, [System.Text.Encoding]::UTF8)
[xml]$doc = $raw
$nsm = New-Object System.Xml.XmlNamespaceManager($doc.NameTable)
$nsm.AddNamespace('ns', 'https://developers.google.com/blockly/xml')
Write-Output ('test1 /ns:xml/ns:block -> ' + $doc.SelectNodes('/ns:xml/ns:block', $nsm).Count)
Write-Output ('test2 //ns:block -> ' + $doc.SelectNodes('//ns:block', $nsm).Count)
Write-Output ('test3 //ns:mutation -> ' + $doc.SelectNodes('//ns:mutation', $nsm).Count)
Write-Output ('test4 GetElementsByTagName block -> ' + $doc.GetElementsByTagName('block').Count)
Write-Output ('test5 root name -> ' + $doc.DocumentElement.Name)
Write-Output ('test6 first child blocks -> ' + $doc.DocumentElement.SelectNodes('ns:block', $nsm).Count)