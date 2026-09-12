$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$origAia = 'd:\projects\cane_controller_2__v2hfg.aia'
$newAia  = 'd:\projects\cane_controller_merged.aia'
if (Test-Path $newAia) { Remove-Item $newAia }
$bkyEntry = 'src/appinventor/ai_zjj44450/cane_controller_2__v2hfg/Screen1.bky'
$scmEntry = 'src/appinventor/ai_zjj44450/cane_controller_2__v2hfg/Screen1.scm'
$bkyBytes = $utf8NoBom.GetBytes([System.IO.File]::ReadAllText("d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.bky", [System.Text.Encoding]::UTF8))
$scmBytes = $utf8NoBom.GetBytes([System.IO.File]::ReadAllText("d:\projects\aia_extracted\src\appinventor\ai_zjj44450\cane_controller_2__v2hfg\Screen1.scm", [System.Text.Encoding]::UTF8))
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
Write-Output ('AIA repacked: ' + (Get-Item $newAia).Length + ' bytes')