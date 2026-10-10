# Downloads the parts of NVIDIA's DLSS SDK that the add-on builds against into third_party\dlss.
# The SDK stays under NVIDIA's license (third_party\dlss\LICENSE.txt) and is not committed to this repo.
param([string]$Version = 'v310.9.1')
$ErrorActionPreference = 'Stop'

$dst = Join-Path $PSScriptRoot '..\third_party\dlss'
$base = "https://raw.githubusercontent.com/NVIDIA/DLSS/$Version"
$headers = gh api "repos/NVIDIA/DLSS/contents/include?ref=$Version" --jq '.[].path' 2>$null
if (-not $headers) {
  $headers = 'include/nvsdk_ngx.h', 'include/nvsdk_ngx_defs.h', 'include/nvsdk_ngx_params.h', 'include/nvsdk_ngx_helpers.h'
}
$files = @{
  'LICENSE.txt' = 'LICENSE.txt'
  'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib' = 'lib/nvsdk_ngx_s.lib'
  'lib/Windows_x86_64/rel/nvngx_dlss.dll' = 'bin/rel/nvngx_dlss.dll'
  'lib/Windows_x86_64/dev/nvngx_dlss.dll' = 'bin/dev/nvngx_dlss.dll'
  # Ray Reconstruction (DLSS-RR), for experiments (docs/raytracing.md)
  'lib/Windows_x86_64/rel/nvngx_dlssd.dll' = 'bin/rel/nvngx_dlssd.dll'
  'lib/Windows_x86_64/dev/nvngx_dlssd.dll' = 'bin/dev/nvngx_dlssd.dll'
}
foreach ($h in $headers) { $files[$h] = $h }

foreach ($src in $files.Keys) {
  $out = Join-Path $dst $files[$src]
  New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
  Invoke-WebRequest -Uri "$base/$src" -OutFile $out -UseBasicParsing
}
Set-Content -Path (Join-Path $dst 'VERSION') -Value $Version
Get-ChildItem $dst -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring((Resolve-Path $dst).Path.Length + 1) } }, Length
