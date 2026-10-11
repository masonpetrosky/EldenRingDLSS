# Packs a release zip from the built add-on: dist\EldenRingDLSS-<version>.zip with an me3 profile, the add-on folder
# (add-on, NVIDIA's release DLSS DLLs, default settings) and the licenses. ReShade is not included; the user adds
# ReShade64.asi (INSTALL.txt). Build first (build.ps1); the version comes from CMakeLists.txt.
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(EldenRingDLSS VERSION ([\d.]+)').Groups[1].Value
$addon = Join-Path $root 'build\EldenRingDLSS.addon64'
if ((Get-Item $addon).VersionInfo.FileVersion -ne $version) { throw "build\EldenRingDLSS.addon64 is not version $version; run build.ps1." }

$name = "EldenRingDLSS-$version"
$stage = Join-Path $root "dist\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$folder = Join-Path $stage 'EldenRingDLSS'
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $folder, $licenses | Out-Null

$package = Join-Path $root 'package'
Copy-Item (Join-Path $package 'EldenRingDLSS.me3') $stage
(Get-Content (Join-Path $package 'INSTALL.txt') -Raw).Replace('@VERSION@', $version) |
  Set-Content (Join-Path $stage 'INSTALL.txt') -Encoding utf8NoBOM -NoNewline
Copy-Item $addon, (Join-Path $package 'EldenRingDLSS.ini'), (Join-Path $package 'ReShade.ini') $folder
Copy-Item (Join-Path $root 'third_party\dlss\bin\rel\nvngx_dlss.dll'), (Join-Path $root 'third_party\dlss\bin\rel\nvngx_dlssd.dll') $folder

Copy-Item (Join-Path $root 'LICENSE') (Join-Path $licenses 'LICENSE.txt')
Copy-Item (Join-Path $root 'third_party\dlss\LICENSE.txt') (Join-Path $licenses 'NVIDIA-RTX-SDKs-LICENSE.txt')
Copy-Item (Join-Path $root 'third_party\reshade\LICENSE.md') (Join-Path $licenses 'ReShade-LICENSE.md')
Copy-Item (Join-Path $root 'third_party\imgui\LICENSE.txt') (Join-Path $licenses 'ImGui-LICENSE.txt')

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Get-Item $zip | Select-Object Name, Length
(Get-FileHash $zip -Algorithm SHA256).Hash
