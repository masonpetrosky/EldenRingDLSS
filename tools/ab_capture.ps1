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
# -Round vegstats: material draw statistics over 120 frames (shots/vegstats.txt) and a 5 s frame-time log.
# -Round wind: motion-vector dumps (4 frames) and 5 s frame-time logs with wind motion vectors off, then on; then back
# to the ini setting. Analyze with tools\wind_check.py.
# -Round winddiag: wind on; a debug capture (shots/winddebug.txt) and motion dump with the game's time step, then with
# a 0.5 s step; frame times with the resolve draw skipped, with everything on, and with wind off.
# -Round windopt: wind on; screenshots with wind on and off (the stream-output copy must draw the grass exactly like
# the game); motion dumps with the game's time step, a 0.5 s step and a 1% depth tolerance; frame times with
# everything on, with vertex-only current passes, without the resolve draw, and with wind off.
# -Round windport: the ported-shader path (WindMode=1): screenshots wind off/on, motion dumps with the game's time
# step and with wind off, frame times with everything on, without the merge, and with wind off.
param(
  [ValidateSet('sharpen', 'presets', 'ca', 'mv', 'vmask', 'vmaskpresets', 'maskcheck', 'vegstats', 'wind',
               'winddiag', 'windopt', 'windport', 'windcov', 'windcost', 'windpixel', 'windprepass')][string]$Round = 'sharpen',
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
} elseif ($Round -eq 'wind') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'wind off' 1.0; Send 'mv 4 windoff'; LogFrames 'wind_off'
  Send 'wind on' 3.0; Send 'windstats' 1.0; Send 'mv 4 windon'; LogFrames 'wind_on'; Send 'windstats'
  Send "wind $restore"
} elseif ($Round -eq 'windpixel') {
  # Where the velocity draw's cost goes: cost modes 0 (normal), 5 (discard-only pixel shader), 6 (no pixel shader),
  # 7 (no alpha test), between two wind-off baselines.
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'wind off' 1.0; LogFrames 'pix_off'
  Send 'wind on' 3.0
  foreach ($mode in 0, 5, 6, 7) { Send "windcost $mode" 1.0; LogFrames "pix_$mode" }
  Send 'windcost 0'; Send 'wind off' 1.0; LogFrames 'pix_off2'
  Send "wind $restore"
} elseif ($Round -eq 'windprepass') {
  # Prepass replacement against the velocity-draw path: coverage markers and motion for both, then frame times
  # (wind off, velocity draws, replaced prepasses, wind off again).
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  $prepass = if ($ini -match '^WindPrepass=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'wind on' 3.0
  foreach ($mode in 'off', 'on') {
    Send "windprepass $mode" 1.0; Send 'windstats' 1.0; Send 'windstats'
    Send 'windmerge drawn' 0.5; Send "mv 1 pre_${mode}_drawn"; Send 'windmerge merged' 0.5; Send "mv 1 pre_${mode}_merged"
    Send 'windmerge on' 0.5; Send "mv 2 pre_${mode}"; Send "capture pre_shot_$mode"
  }
  Send 'wind off' 1.0; LogFrames 'pre_wind_off'
  Send 'wind on'; Send 'windprepass off' 3.0; LogFrames 'pre_draws'
  Send 'windprepass on' 1.0; LogFrames 'pre_replaced'
  Send 'wind off' 1.0; LogFrames 'pre_wind_off2'
  Send "windprepass $prepass"; Send "wind $restore"
} elseif ($Round -eq 'windcost') {
  # Ported path cost breakdown: frame times per cost mode (0 fused, 1 unfused, 2 no wind math, 3 switches only,
  # 4 game draw only), between two wind-off baselines; motion dumps for the fused and unfused shaders.
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'wind off' 1.0; LogFrames 'cost_off'
  Send 'wind on' 3.0; Send 'windcost 0' 1.0; Send 'mv 2 cost_fused'; LogFrames 'cost_0'
  Send 'windcost 1' 1.0; Send 'mv 2 cost_unfused'; LogFrames 'cost_1'
  foreach ($mode in 2, 3, 4) { Send "windcost $mode" 1.0; LogFrames "cost_$mode" }
  Send 'windcost 0'; Send 'wind off' 1.0; LogFrames 'cost_off2'
  Send "wind $restore"
} elseif ($Round -eq 'windcov') {
  # Ported path: coverage markers (pixels the vegetation pass drew, pixels the merge kept), real motion, frame times.
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'wind on' 3.0; Send 'windstats' 1.0
  Send 'windmerge drawn' 0.5; Send 'mv 1 cov_drawn'; Send 'windmerge merged' 0.5; Send 'mv 1 cov_merged'
  Send 'windmerge on' 0.5; Send 'mv 2 port_on'; Send 'windstats'
  LogFrames 'port_on'; Send 'wind off' 1.0; LogFrames 'port_off'
  Send "wind $restore"
} elseif ($Round -eq 'windport') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'wind off' 1.0; Send 'capture port_shot_off'; Send 'mv 2 port_off'
  Send 'wind on' 3.0; Send 'capture port_shot_on'; Send 'windstats' 1.0; Send 'mv 2 port_on'; Send 'windstats'
  LogFrames 'port_on'
  Send 'windmerge off' 1.0; LogFrames 'port_nomerge'; Send 'windmerge on'
  Send 'wind off' 1.0; LogFrames 'port_off'
  Send "wind $restore"
} elseif ($Round -eq 'windopt') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  Remove-Item (Join-Path $Folder 'shots\winddebug.txt') -ErrorAction SilentlyContinue
  Send 'dlss on'; Send 'wind off' 1.0; Send 'capture wind_shot_off'
  Send 'wind on' 5.0; Send 'capture wind_shot_on'; Send 'windstats' 1.0; Send 'winddebug'
  Send 'mv 2 wind_dt'; Send 'windstats'
  Send 'windtime 0.5' 1.0; Send 'mv 2 wind_half'; Send 'windtime 0'
  Send 'winddepth 0.01' 1.0; Send 'mv 2 wind_tol'; Send 'winddepth 0.0001'
  LogFrames 'wind_on3'
  Send 'windfull off' 1.0; LogFrames 'wind_vsonly'; Send 'windfull on'
  Send 'windresolve off' 1.0; LogFrames 'wind_noresolve'; Send 'windresolve on'
  Send 'wind off' 1.0; LogFrames 'wind_off3'
  Send "wind $restore"
} elseif ($Round -eq 'winddiag') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  Remove-Item (Join-Path $Folder 'shots\winddebug.txt') -ErrorAction SilentlyContinue
  Send 'dlss on'; Send 'wind on' 3.0; Send 'windstats' 1.0; Send 'winddebug'; Send 'mv 2 wind_dt'; Send 'windstats'
  Send 'windtime 0.5' 1.0; Send 'winddebug'; Send 'mv 2 wind_half'; Send 'windtime 0'
  Send 'windresolve off' 1.0; LogFrames 'wind_so_only'
  Send 'windresolve on' 1.0; LogFrames 'wind_on2'
  Send 'wind off' 1.0; LogFrames 'wind_off2'
  Send "wind $restore"
} elseif ($Round -eq 'vegstats') {
  Send 'vegstats 120'; LogFrames 'veg_baseline'
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
