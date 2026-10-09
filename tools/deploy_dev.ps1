# Copies the built add-on, ReShade and the DLSS runtime into ModEngine3's profile folder and writes the
# `eldenring-dlss-dev` profile (UnlockTheFps + ReShade + this add-on). Nothing is written to the game folder.
param(
  [string]$ReShadeDll = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH\End\Binaries\Win64\dxgi.dll',
  [ValidateSet('dev', 'rel')][string]$DlssBuild = 'dev'
)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$profiles = Join-Path $env:LOCALAPPDATA 'garyttierney\me3\config\profiles'
$dst = Join-Path $profiles 'natives\EldenRingDLSS'
New-Item -ItemType Directory -Force $dst | Out-Null

if ((Get-Item $ReShadeDll).VersionInfo.ProductName -ne 'ReShade') { throw "$ReShadeDll is not ReShade." }
# The .asi name makes ReShade use its own folder as base path while still hooking D3D12 and DXGI.
Copy-Item $ReShadeDll (Join-Path $dst 'ReShade64.asi') -Force
Copy-Item (Join-Path $root 'build\EldenRingDLSS.addon64') $dst -Force
Copy-Item (Join-Path $root "third_party\dlss\bin\$DlssBuild\nvngx_dlss.dll") $dst -Force

$ini = Join-Path $dst 'ReShade.ini'
if (-not (Test-Path $ini)) {
  @'
[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\**
TextureSearchPaths=.\reshade-shaders\Textures\**
PresetPath=.\ReShadePreset.ini
PerformanceMode=1

[INPUT]
KeyOverlay=36,0,0,0

[OVERLAY]
TutorialProgress=4
'@ | Set-Content -Path $ini -Encoding ascii
}

@"
profileVersion = "v1"

[[supports]]
game = "eldenring"

[[natives]]
enabled = true
load_early = true
initializer = { function = "unlock_the_fps_init" }
path = 'natives/UnlockTheFps/UnlockTheFps.dll'

[[natives]]
enabled = true
load_early = true
path = 'natives/EldenRingDLSS/ReShade64.asi'
"@ | Set-Content -Path (Join-Path $profiles 'eldenring-dlss-dev.me3') -Encoding utf8NoBOM

Get-ChildItem $dst | Select-Object Name, Length, LastWriteTime
