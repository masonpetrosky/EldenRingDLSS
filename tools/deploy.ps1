# Installs the built add-on, ReShade and the DLSS runtime into ModEngine3's profile folder. Nothing is written
# to the game folder.
#   -Target main  natives\EldenRingDLSS with the release DLSS DLL, added to the everyday `eldenring-default`
#                 profile (Start menu "ELDEN RING (me3)") after UnlockTheFps.
#   -Target dev   natives\EldenRingDLSS-dev with NVIDIA's developer DLSS DLL, in its own `eldenring-dlss-dev`
#                 profile, so test builds never touch the everyday setup.
param(
  [Parameter(Mandatory)][ValidateSet('main', 'dev')][string]$Target,
  [string]$ReShadeDll = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH\End\Binaries\Win64\dxgi.dll'
)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$profiles = Join-Path $env:LOCALAPPDATA 'garyttierney\me3\config\profiles'
$folder = if ($Target -eq 'main') { 'EldenRingDLSS' } else { 'EldenRingDLSS-dev' }
$dlssBuild = if ($Target -eq 'main') { 'rel' } else { 'dev' }
$dst = Join-Path $profiles "natives\$folder"
New-Item -ItemType Directory -Force $dst | Out-Null

if ((Get-Item $ReShadeDll).VersionInfo.ProductName -ne 'ReShade') { throw "$ReShadeDll is not ReShade." }
# The .asi name makes ReShade use its own folder as base path while still hooking D3D12 and DXGI.
Copy-Item $ReShadeDll (Join-Path $dst 'ReShade64.asi') -Force
Copy-Item (Join-Path $root 'build\EldenRingDLSS.addon64') $dst -Force
Copy-Item (Join-Path $root "third_party\dlss\bin\$dlssBuild\nvngx_dlss.dll") $dst -Force

$reshadeIni = Join-Path $dst 'ReShade.ini'
if (-not (Test-Path $reshadeIni)) {
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
'@ | Set-Content -Path $reshadeIni -Encoding ascii
}
$addonIni = Join-Path $dst 'EldenRingDLSS.ini'
if (-not (Test-Path $addonIni)) {
  "[EldenRingDLSS]`r`nEnabled=1`r`nGameSharpening=0`r`nGameExposure=1`r`n" | Set-Content -Path $addonIni -Encoding ascii
}

$native = @"
[[natives]]
enabled = true
load_early = true
path = 'natives/$folder/ReShade64.asi'
"@

if ($Target -eq 'dev') {
  @"
profileVersion = "v1"

[[supports]]
game = "eldenring"

[[natives]]
enabled = true
load_early = true
initializer = { function = "unlock_the_fps_init" }
path = 'natives/UnlockTheFps/UnlockTheFps.dll'

$native
"@ | Set-Content -Path (Join-Path $profiles 'eldenring-dlss-dev.me3') -Encoding utf8NoBOM
} else {
  $profile = Join-Path $profiles 'eldenring-default.me3'
  $text = Get-Content $profile -Raw
  if ($text -notmatch [regex]::Escape("natives/$folder/ReShade64.asi")) {
    Copy-Item $profile "$profile.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
    # Insert before the trailing [debug_properties] table if there is one, so the natives stay together.
    $text = if ($text -match '(?m)^\[debug_properties\]') {
      $text -replace '(?m)^\[debug_properties\]', "$native`r`n`r`n[debug_properties]"
    } else {
      $text.TrimEnd() + "`r`n`r`n$native`r`n"
    }
    Set-Content -Path $profile -Value $text -Encoding utf8NoBOM -NoNewline
  }
}

Get-ChildItem $dst -File | Select-Object Name, Length, LastWriteTime
