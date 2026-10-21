# Downloads the parts of NVIDIA's Streamline SDK that DLSS Frame Generation needs into third_party\streamline: the
# headers, the signed production DLLs (sl.interposer, sl.common, sl.dlss_g, sl.reflex, sl.pcl and nvngx_dlssg) and
# their licenses. Streamline itself is MIT-licensed (LICENSE.txt); nvngx_dlssg.dll is under the NVIDIA RTX SDKs license
# like the DLSS DLLs, sl.reflex under the NVIDIA SDK license (reflex.license.txt). Like the DLSS SDK they are not
# committed to this repo. -Zip uses an already downloaded release zip instead of downloading it (276 MB).
param(
  [string]$Version = 'v2.14.16',
  [string]$Sha256 = '32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B',
  [string]$Zip = '',
  [string]$Destination = (Join-Path $PSScriptRoot '..\third_party\streamline')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$dst = [System.IO.Path]::GetFullPath($Destination)
$downloaded = $false
if (-not $Zip) {
  $Zip = Join-Path ([System.IO.Path]::GetTempPath()) "streamline-sdk-$Version.zip"
  $url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/$Version/streamline-sdk-$Version.zip"
  Invoke-WebRequest -Uri $url -OutFile $Zip -UseBasicParsing
  $downloaded = $true
}
try {
  $hash = (Get-FileHash $Zip -Algorithm SHA256).Hash
  if ($hash -ne $Sha256) { throw "streamline-sdk-$Version.zip has SHA-256 $hash, expected $Sha256." }

  $bins = 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll'
  $files = @{
    'license.txt' = 'LICENSE.txt'
    'bin/x64/nvngx_dlss.license.txt' = 'NVIDIA-RTX-SDKs-LICENSE.txt'
    'bin/x64/reflex.license.txt' = 'reflex.license.txt'
  }
  foreach ($b in $bins) { $files["bin/x64/$b"] = "bin/$b" }

  if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
  $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
  try {
    foreach ($entry in $archive.Entries) {
      $out = $null
      if ($files.ContainsKey($entry.FullName)) { $out = $files[$entry.FullName] }
      elseif ($entry.FullName -match '^include/[^/]+\.h$') { $out = $entry.FullName }
      if (-not $out) { continue }
      $path = Join-Path $dst $out
      New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
      [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $path, $true)
    }
  } finally {
    $archive.Dispose()
  }
  foreach ($b in $bins) {
    $signature = Get-AuthenticodeSignature (Join-Path $dst "bin/$b")
    if ($signature.Status -ne 'Valid') { throw "$b is not validly signed ($($signature.Status))." }
  }
} finally {
  if ($downloaded) { Remove-Item $Zip -Force -ErrorAction SilentlyContinue }
}
Set-Content -Path (Join-Path $dst 'VERSION') -Value $Version
Get-ChildItem $dst -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring($dst.Length + 1) } }, Length
