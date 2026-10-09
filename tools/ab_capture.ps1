# Captures the same spot with several anti-aliasing settings through the add-on's remote control.
# Stand still in game first. Writes shots to <add-on folder>\shots and restores DLAA without sharpening.
# -Round sharpen: DLAA with sharpening 0/0.10/0.25, preset J, game TAA.
# -Round presets: DLAA presets K/M/L, sharpening 0.15, game TAA, each with a 5 s PresentMon frame-time log
# (shots\<name>.csv).
# -Round ca: the game's chromatic aberration on and off (measure with tools\ca_measure.py), then back to the
# ini setting.
# -Round mv: two dumps of the TAA's motion vectors and color (8 frames each, 1 s apart) for tools\mv_check.py.
# Keep the camera at rest so only objects that write their own velocity show motion.
# -Round vmask: DLAA with the vegetation bias mask off, on the whole screen (does the preset read the mask at all?),
# and on vegetation at strength 1 and 0.5; then back to the ini setting.
# -Round vmaskpresets: the whole-screen mask off and on with presets F (CNN, documented to read the mask), L and M,
# then back to the ini preset with the mask off.
# -Round maskcheck: dumps the vegetation mask (whole screen, then vegetation only) next to the stencil.
param(
  [ValidateSet('sharpen', 'presets', 'ca', 'mv', 'vmask', 'vmaskpresets', 'maskcheck')][string]$Round = 'sharpen',
  [string]$Folder = "$env:LOCALAPPDATA\garyttierney\me3\config\profiles\natives\EldenRingDLSS-dev",
  [string]$PresentMon = "$env:USERPROFILE\Documents\AC2HighFPS\.presentmon\2.5.1\PresentMon-2.5.1-x64.exe"
)
$ErrorActionPreference = 'Stop'

function Send([string]$Command, [double]$Settle = 0) {
  $done = Join-Path $Folder 'remote.done'
  Remove-Item $done -ErrorAction SilentlyContinue
  Set-Content -Path (Join-Path $Folder 'remote.txt') -Value $Command -Encoding ascii
  for ($i = 0; $i -lt 300 -and -not (Test-Path $done); $i++) { Start-Sleep -Milliseconds 100 }
  if (-not (Test-Path $done)) { throw "No response to '$Command' (is the game running with the dev build?)" }
  Get-Content $done
  if ($Settle -gt 0) { Start-Sleep -Milliseconds ([int]($Settle * 1000)) }
}

function LogFrames([string]$Name) {
  $csv = Join-Path $Folder "shots\$Name.csv"
  Remove-Item $csv -ErrorAction SilentlyContinue
  & $PresentMon --process_name eldenring.exe --output_file $csv --timed 5 --terminate_after_timed `
    --stop_existing_session --no_console_stats | Out-Null
  "measured $Name"
}

if ($Round -eq 'sharpen') {
  Send 'dlss on'; Send 'sharpen off' 1.5; Send 'capture dlaa'
  Send 'sharpen on'; Send 'strength 0.25' 0.5; Send 'capture dlaa_sharp25'
  Send 'strength 0.10' 0.5; Send 'capture dlaa_sharp10'
  Send 'sharpen off'; Send 'preset 10' 2.5; Send 'capture dlaa_presetJ'
  Send 'preset 0'; Send 'dlss off' 2.0; Send 'capture taa'
  Send 'dlss on' 1.0
} elseif ($Round -eq 'vmask') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = ($ini | Where-Object { $_ -match '^VegetationMask=' }) -replace '^VegetationMask=', ''
  if (-not $restore) { $restore = '0' }
  Send 'dlss on'; Send 'vmaskall off'; Send 'vmask 0' 1.5; Send 'capture vm_off'
  Send 'vmaskall on'; Send 'vmask 1' 1.5; Send 'capture vm_all'
  Send 'vmaskall off' 1.5; Send 'capture vm_veg100'
  Send 'vmask 0.5' 1.5; Send 'capture vm_veg50'
  Send 'vmask 0' 1.5; Send 'capture vm_off2'
  Send "vmask $restore"
} elseif ($Round -eq 'vmaskpresets') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $preset = ($ini | Where-Object { $_ -match '^Preset=' }) -replace '^Preset=', ''
  if (-not $preset) { $preset = '0' }
  Send 'dlss on'; Send 'vmaskall on'
  foreach ($p in @(@('6', 'F'), @('12', 'L'), @('13', 'M'))) {
    Send 'vmask 0'; Send "preset $($p[0])" 3.0; Send "capture vmp_$($p[1])_off"
    Send 'vmask 1' 1.5; Send "capture vmp_$($p[1])_all"
  }
  Send 'vmask 0'; Send 'vmaskall off'; Send "preset $preset" 1.0
} elseif ($Round -eq 'maskcheck') {
  Send 'dlss on'; Send 'vmaskall on'; Send 'vmask 1' 0.5; Send 'mv 2 maskall'
  Send 'vmaskall off' 0.5; Send 'mv 2 maskveg'
  Send 'vmask 0'
} elseif ($Round -eq 'mv') {
  Send 'mv 8 still_a' 1.0; Send 'mv 8 still_b'
} elseif ($Round -eq 'ca') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^ChromaticAberration=0') { 'off' } else { 'on' }
  Send 'ca on' 0.5; Send 'capture ca_on'
  Send 'ca off' 0.5; Send 'capture ca_off'
  Send "ca $restore"
} else {
  Send 'dlss on'; Send 'sharpen off'; Send 'preset 0' 2.5; Send 'capture dlaa'; LogFrames 'dlaa'
  Send 'preset 13' 2.5; Send 'capture dlaa_presetM'; LogFrames 'dlaa_presetM'
  Send 'preset 12' 2.5; Send 'capture dlaa_presetL'; LogFrames 'dlaa_presetL'
  Send 'preset 0' 2.5; Send 'sharpen on'; Send 'strength 0.15' 0.5; Send 'capture dlaa_sharp15'
  Send 'sharpen off'; Send 'dlss off' 2.0; Send 'capture taa'; LogFrames 'taa'
  Send 'dlss on' 1.0
}
