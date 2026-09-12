$ErrorActionPreference = 'Stop'
$bkyPath = "d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky"
$scmPath = "d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.scm"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Esc($str) { (($str.ToCharArray() | ForEach-Object { if ([int]$_ -gt 127) { '\u{0:x4}' -f [int]$_ } else { "$_" } }) -join '') }

# ============ 1. modify Screen1.bky ============
$bky = [System.IO.File]::ReadAllText($bkyPath, [System.Text.Encoding]::UTF8)

# 1a. change hint text in AfterPicking
$oldHint = '<field name="TEXT">识别结果：已选择图片，请开始识别</field>'
$newHint = '<field name="TEXT">识别中，请稍候……</field>'
$c1 = ([regex]::Matches($bky, [regex]::Escape($oldHint))).Count
if ($c1 -ne 1) { throw "hint anchor count = $c1 (expect 1)" }
$bky = $bky.Replace($oldHint, $newHint)

# 1b. append set Url + PostFile after the confidence-reset block in AfterPicking
$anchor = '置信度：--</field></block></value></block></next></block></next></block></next></block></statement></block>'
$newFrag = '置信度：--</field></block></value></block>' +
  '<next><block type="component_set_get" id="mrg0seturl0x0001"><mutation xmlns="http://www.w3.org/1999/xhtml" component_type="Web" set_or_get="set" property_name="Url" is_generic="false" instance_name="交通灯识别接口"></mutation><field name="COMPONENT_SELECTOR">交通灯识别接口</field><field name="PROP">Url</field><value name="VALUE"><block type="lexical_variable_get" id="mrg0geturl0x0002"><field name="VAR">global TrafficURL</field></block></value>' +
  '<next><block type="component_method" id="mrg0postfl0x0003"><mutation xmlns="http://www.w3.org/1999/xhtml" component_type="Web" method_name="PostFile" is_generic="false" instance_name="交通灯识别接口"></mutation><field name="COMPONENT_SELECTOR">交通灯识别接口</field><value name="ARG0"><block type="lexical_variable_get" id="mrg0getimg0x0004"><field name="VAR">global selectedTrafficImage</field></block></value></block></next></block></next>' +
  '</next></block></next></block></next></block></statement></block>'
$c2 = ([regex]::Matches($bky, [regex]::Escape($anchor))).Count
if ($c2 -ne 1) { throw "tail anchor count = $c2 (expect 1)" }
$bky = $bky.Replace($anchor, $newFrag)

# validate XML
[xml]$null = $bky
[System.IO.File]::WriteAllText($bkyPath, $bky, $utf8NoBom)
Write-Output 'BKY updated OK'

# ============ 2. modify Screen1.scm ============
$scm = [System.IO.File]::ReadAllText($scmPath, [System.Text.Encoding]::UTF8)

# 2a. ImagePicker text: 导入图片 -> 导入并识别
$a1 = '"Text":"' + (Esc '导入图片') + '","TextColor"'
$n1 = '"Text":"' + (Esc '导入并识别') + '","TextColor"'
$d1 = ([regex]::Matches($scm, [regex]::Escape($a1))).Count
if ($d1 -ne 1) { throw "scm anchor1 count = $d1 (expect 1)" }
$scm = $scm.Replace($a1, $n1)

# 2b. instruction label text
$oldIns = Esc '导入道路图片后点击“开始识别”。识别结果可通过语音播报。'
$newIns = Esc '点击“导入并识别”选择道路图片，系统将自动识别并语音播报结果。'
$a2 = '"Text":"' + $oldIns + '"'
$n2 = '"Text":"' + $newIns + '"'
$d2 = ([regex]::Matches($scm, [regex]::Escape($a2))).Count
if ($d2 -ne 1) { throw "scm anchor2 count = $d2 (expect 1)" }
$scm = $scm.Replace($a2, $n2)

# validate JSON
$jstart = $scm.IndexOf('{'); $jend = $scm.LastIndexOf('}')
$null = (($scm.Substring($jstart, $jend - $jstart + 1)) | ConvertFrom-Json)
[System.IO.File]::WriteAllText($scmPath, $scm, $utf8NoBom)
Write-Output 'SCM updated OK'

# ============ 3. repackage .aia ============
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$origAia = 'd:\projects\cane_controller_2__v2hfg.aia'
$newAia  = 'd:\projects\cane_controller_merged.aia'
if (Test-Path $newAia) { Remove-Item $newAia }
$bkyEntry = 'src/appinventor/ai_zjj44450/cane_controller_2__v2hfg/Screen1.bky'
$scmEntry = 'src/appinventor/ai_zjj44450/cane_controller_2__v2hfg/Screen1.scm'
$bkyBytes = $utf8NoBom.GetBytes($bky)
$scmBytes = $utf8NoBom.GetBytes($scm)
$fs = [System.IO.File]::OpenRead($origAia)
$zin = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Read)
$ofs = [System.IO.File]::Create($newAia)
$zout = New-Object System.IO.Compression.ZipArchive($ofs, [System.IO.Compression.ZipArchiveMode]::Create)
foreach ($e in $zin.Entries) {
  $ne = $zout.CreateEntry($e.FullName)
  $os = $ne.Open()
  if ($e.FullName -eq $bkyEntry) { $os.Write($bkyBytes, 0, $bkyBytes.Length) }
  elseif ($e.FullName -eq $scmEntry) { $os.Write($scmBytes, 0, $scmBytes.Length) }
  else { $is = $e.Open(); $is.CopyTo($os); $is.Dispose() }
  $os.Dispose()
}
$zout.Dispose(); $zin.Dispose(); $ofs.Dispose(); $fs.Dispose()
Write-Output ('AIA written: ' + $newAia + ' (' + (Get-Item $newAia).Length + ' bytes)')