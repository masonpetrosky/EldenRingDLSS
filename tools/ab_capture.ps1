# Captures the same spot with several anti-aliasing settings through the add-on's remote control.
# Stand still in game first. Writes shots to <add-on folder>\shots and restores DLAA without sharpening.
# -Round sharpen: DLAA with sharpening 0/0.10/0.25, preset J, game TAA.
# -Round presets: super resolution presets K, M, L and E at the current render scale (Ray Reconstruction off, it has
# its own model), each with a screenshot, a 5 s frame-time log and a 30-frame GPU profile (tools\gpu_profile.py
# shots\gpuprofile_pr_*.csv); then back to the ini's preset and Ray Reconstruction setting.
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
# -Round upscale: render scale 0.67 with DLSS at the TAA and full-size post-processing: status, a pass log, a
# screenshot and frame times; the same without the corrections; then DLAA (scale 1) and the game TAA.
# -Round windport: the ported-shader path (WindMode=1): screenshots wind off/on, motion dumps with the game's time
# step and with wind off, frame times with everything on, without the merge, and with wind off.
# -Round showcase: README comparison: DLSS at render scale 0.50 and 0.67, DLAA and the game TAA, each a screenshot and
# a 5 s frame-time log. Start the game with RenderScale=0.50 (upscaling needs a scale below 1 at startup); ends at 0.50.
# -Round rrshow: Ray Reconstruction for the README and the Nexus page, at render scale 0.50: DLSS super resolution with
# the game's denoisers, Ray Reconstruction with the add-on's rays and with the game's rays, and the game's TAA at full
# size; each a screenshot and a 5 s frame-time log, the DLSS ones also a 30-frame GPU profile. Then back to the ini's
# RayReconstruction and RtShadowRayGen; ends at 0.50.
# -Round vanilla: the unmodified game for the README's tables: the game's TAA at full size with the add-on's motion
# vectors, object blur and Reflex off, chromatic aberration on and the game's refit schedule (every structure every
# frame); a screenshot and a 5 s frame-time log. Then back to the ini's settings and the default schedule; ends at
# 0.50 with DLSS on.
# -Round mvcost: what the add-on's motion vectors cost at the current DLSS settings: 5 s frame-time logs with
# everything on, tree motion off, wind (grass and trees) off, and wind and NPC motion off; then back to the ini.
# -Round water: the native-size water reflections (WaterReflections) at a still camera on water: 8-shot bursts with
# them on and off at the current render scale and with the game's TAA at full size (temporal noise: tools\flicker.py),
# and a 5 s frame-time log and a 30-frame GPU profile on and off (tools\gpu_profile.py --compare). Shots go to
# shots\water; ends with DLSS at 0.50 and the ini's setting.
# -Round perf: where the frame goes at a slow spot (see the round); restores the ini's settings.
# -Round fg: frame generation off, 2x, 3x and 4x at the current spot (see the round); restores the ini's setting.
# -Round treebatch: tree wind patches before their lists, inline, and trees off at the current spot (see the round).
# -Round treecheck: checks the tree wind patches that run before their lists at their draws, and an A/B of both
# placements in neighboring frames (see the round).
# -Round rtfoliage: GPU profiles and screenshots of our rays with and without the foliage alpha test, shorter AO rays
# and nearer opaque shadows (see the round); restores the defaults.
param(
  [ValidateSet('sharpen', 'presets', 'ca', 'mv', 'vmask', 'vmaskpresets', 'maskcheck', 'vegstats', 'wind',
               'winddiag', 'windopt', 'windport', 'windcov', 'windcost', 'windpixel', 'windprepass', 'survey', 'treedump', 'trees', 'treecost', 'drsprobe', 'upscale',
               'showcase', 'showcasert', 'rrshow', 'vanilla', 'mvcost', 'rtfoliage', 'water', 'perf', 'fg', 'treebatch', 'treecheck')][string]$Round = 'sharpen',
  [string]$Folder = "$env:LOCALAPPDATA\garyttierney\me3\config\profiles\natives\EldenRingDLSS-dev",
  [string]$PresentMon = "$env:USERPROFILE\Documents\Tools\PresentMon\2.5.1\PresentMon-2.5.1-x64.exe"
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
} elseif ($Round -eq 'trees') {
  # Tree motion: what still moves without motion vectors with trees off and on (two 8-frame dumps a second apart
  # each, mv_check.py --compare), screenshots, then frame times (wind off, grass only, grass and trees).
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  $trees = if ($ini -match '^WindTrees=0') { 'off' } else { 'on' }
  Send 'dlss on'; Send 'wind on' 3.0
  foreach ($mode in 'off', 'on') {
    Send "windtrees $mode" 2.0; Send 'windstats' 1.0; Send 'windstats'
    Send "mv 8 tree_${mode}_a" 1.0; Send "mv 8 tree_${mode}_b"; Send "capture tree_shot_$mode"
  }
  Send 'wind off' 1.0; LogFrames 'tree_wind_off'
  Send 'wind on'; Send 'windtrees off' 3.0; LogFrames 'tree_grass_only'
  Send 'windtrees on' 2.0; LogFrames 'tree_grass_trees'
  Send 'wind off' 1.0; LogFrames 'tree_wind_off2'
  Send "windtrees $trees"; Send "wind $restore"
} elseif ($Round -eq 'drsprobe') {
  # Stage 3: set every float equal to 1.0 in a few engine objects to 0.67 for a few frames and report what changes
  # in the frame's resolution constants (src/drs_probe.hpp). Reports: shots/drsprobe_<name>.txt.
  Send 'dlss on'; Send 'drsstatus' 1.0; Send 'drsstatus'
  $roots = [ordered]@{
    'gm' = @('drsptr 47f37a8 0', '14000')                    # the graphics manager
    'gm13a60' = @('drsptr 47f37a8 13a60 0', '400')
    'gm13a68' = @('drsptr 47f37a8 13a68 0', '400')
    'context' = @('drsptr 3d61f98 8 0', '1000')              # the render context of the view setup
  }
  foreach ($name in $roots.Keys) {
    $answer = Send $roots[$name][0]
    if ("$answer" -notmatch 'address (0x[0-9a-f]+)') { "skipping ${name}: $answer"; continue }
    $address = $Matches[1].Substring(2)
    Send "drsprobe $address $($roots[$name][1])"
    for ($i = 0; $i -lt 600; $i++) {
      Start-Sleep -Seconds 1
      $status = Send 'drsstatus'
      if ("$status" -match 'probe idle') { break }
    }
    Copy-Item (Join-Path $Folder 'shots\drsprobe.txt') (Join-Path $Folder "shots\drsprobe_$name.txt") -Force
    Get-Content (Join-Path $Folder "shots\drsprobe_$name.txt") | Select-Object -First 12
  }
} elseif ($Round -eq 'upscale') {
  # Stage 3: DLSS at the TAA with the passes after it corrected to full size (CorrectPostPass, src/stretch.hpp).
  function SavePassLog([string]$Name) {
    Send 'passlog' 1.5
    Copy-Item (Join-Path $Folder 'shots\passlog.txt') (Join-Path $Folder "shots\passlog_$Name.txt") -Force
  }
  Send 'dlss on'; Send 'scale 0.67' 5.0; Send 'scale'; Send 'postinfo'
  SavePassLog 'up67'; Send 'capture up67'; LogFrames 'up67'
  Send 'postfix off' 2.0; SavePassLog 'up67_nofix'; Send 'capture up67_nofix'; Send 'postfix on' 2.0
  Send 'scale 1' 5.0; Send 'scale'; Send 'capture dlaa'; LogFrames 'dlaa'
  Send 'dlss off' 2.0; Send 'capture taa'; LogFrames 'taa'
  Send 'dlss on'; Send 'scale 0.67' 5.0; Send 'scale'; Send 'postinfo'
} elseif ($Round -eq 'presets') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = ($ini | Where-Object { $_ -match '^Preset=' }) -replace '^Preset=', ''
  if (-not $restore) { $restore = '0' }
  $rr = if ($ini -match '^RayReconstruction=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'rr off'
  foreach ($p in ([ordered]@{ K = 11; M = 13; L = 12; E = 5 }).GetEnumerator()) {
    Send "preset $($p.Value)" 4.0
    Send "capture pr_$($p.Key)"; LogFrames "pr_$($p.Key)"; Send "gpuprofile 30 pr_$($p.Key)"
  }
  Send "preset $restore"; Send "rr $rr"
} elseif ($Round -eq 'showcase') {
  Send 'dlss on'; Send 'scale 0.5' 5.0; Send 'scale'; Send 'capture show_dlss50'; LogFrames 'show_dlss50'
  Send 'scale 0.67' 5.0; Send 'scale'; Send 'capture show_dlss67'; LogFrames 'show_dlss67'
  Send 'scale 1' 5.0; Send 'scale'; Send 'capture show_dlaa'; LogFrames 'show_dlaa'
  Send 'dlss off' 2.0; Send 'capture show_taa'; LogFrames 'show_taa'
  Send 'dlss on'; Send 'scale 0.5' 5.0; Send 'scale'
} elseif ($Round -eq 'showcasert') {
  # The showcase's ray tracing baseline: the game's TAA with the add-on's refit schedule off (every structure refit
  # every frame, as the game does), then on, then DLSS at 0.50. Restores the default schedule (rtdist 20 50, rtrefit 2).
  Send 'dlss on'; Send 'scale 1' 5.0; Send 'dlss off'; Send 'rtdist 0'; Send 'rtrefit 1' 3.0
  Send 'capture showrt_vanilla'; LogFrames 'showrt_vanilla'
  Send 'rtdist 20 50'; Send 'rtrefit 2' 3.0; LogFrames 'showrt_taa'
  Send 'dlss on'; Send 'scale 0.5' 5.0; Send 'scale'; LogFrames 'showrt_dlss50'
} elseif ($Round -eq 'rrshow') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $rr = if ($ini -match '^RayReconstruction=1') { 'on' } else { 'off' }
  $rtgen = ($ini | Where-Object { $_ -match '^RtShadowRayGen=' }) -replace '^RtShadowRayGen=', ''
  if (-not $rtgen) { $rtgen = '3' }
  Send 'dlss on'; Send 'scale 0.5' 5.0; Send 'scale'; Send 'rtgen 3'; Send 'rr off' 4.0; Send 'rr'
  Send 'capture rr_sr'; LogFrames 'rr_sr'; Send 'gpuprofile 30 rr_sr'
  Send 'rr on' 6.0; Send 'rr'; Send 'rtgen'
  Send 'capture rr_rr'; LogFrames 'rr_rr'; Send 'gpuprofile 30 rr_rr'
  Send 'rtgen 0' 4.0; Send 'capture rr_gamerays'; LogFrames 'rr_gamerays'; Send 'gpuprofile 30 rr_gamerays'
  Send "rtgen $rtgen"; Send "rr $rr"
  Send 'scale 1' 5.0; Send 'dlss off' 2.0; Send 'capture rr_taa'; LogFrames 'rr_taa'
  Send 'dlss on'; Send 'scale 0.5' 5.0; Send 'scale'
} elseif ($Round -eq 'vanilla') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $state = @{}
  foreach ($key in 'WindMotion', 'NpcMotion', 'ObjectBlur', 'Reflex', 'ChromaticAberration') {
    $state[$key] = if ($ini -match "^$key=0") { 'off' } else { 'on' }
  }
  Send 'dlss on'; Send 'scale 1' 5.0; Send 'dlss off'; Send 'wind off'; Send 'npc off'; Send 'objblur off'
  Send 'reflex off'; Send 'ca on'; Send 'rtdist 0'; Send 'rtrefit 1' 3.0
  Send 'capture vanilla'; LogFrames 'vanilla'
  Send 'rtdist 20 50'; Send 'rtrefit 2'; Send "wind $($state.WindMotion)"; Send "npc $($state.NpcMotion)"
  Send "objblur $($state.ObjectBlur)"; Send "reflex $($state.Reflex)"; Send "ca $($state.ChromaticAberration)"
  Send 'dlss on'; Send 'scale 0.5' 5.0; Send 'scale'
} elseif ($Round -eq 'perf') {
  # Where the frame goes at a slow spot: the ini's settings with a 5 s frame-time log and a 60-frame GPU profile, then
  # one thing off at a time (a 5 s log each), then the same spot at render scale 0.33 (what still costs at a smaller
  # render size) and with the game's TAA at full size. Summarize with tools\frames.py shots\perf_*.csv and
  # tools\gpu_profile.py shots\gpuprofile_perf_base.csv. Restores the ini's settings; ends at 0.50.
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  function IniSwitch([string]$Key, [string]$Default = 'on') {
    if ($ini -match "^$Key=1") { 'on' } elseif ($ini -match "^$Key=0") { 'off' } else { $Default }
  }
  $state = [ordered]@{ rr = IniSwitch 'RayReconstruction'; ssr = IniSwitch 'WaterReflections'
    windtrees = IniSwitch 'WindTrees'; wind = IniSwitch 'WindMotion'; npc = IniSwitch 'NpcMotion'
    objblur = IniSwitch 'ObjectBlur'; reflex = IniSwitch 'Reflex' }
  $rtgen = ($ini | Where-Object { $_ -match '^RtShadowRayGen=' }) -replace '^RtShadowRayGen=', ''
  if (-not $rtgen) { $rtgen = '4' }
  $scale = ($ini | Where-Object { $_ -match '^RenderScale=' }) -replace '^RenderScale=', ''
  if (-not $scale) { $scale = '0.5' }
  # Rendered frames only: frame generation off meanwhile.
  $fg = ($ini | Where-Object { $_ -match '^FrameGeneration=' }) -replace '^FrameGeneration=', ''
  if ($fg -and $fg -ne '0') { Send 'fg 0' 2.0 }
  Send 'dlss on' 3.0; Send 'rr'; Send 'scale'; LogFrames 'perf_base'; Send 'gpuprofile 60 perf_base' 3.0
  Send 'reflex'
  if ($state.rr -eq 'on') {
    Send 'rtgen 0' 4.0; LogFrames 'perf_gamerays'; Send "rtgen $rtgen"
    Send 'rr off' 5.0; LogFrames 'perf_norr'; Send 'gpuprofile 60 perf_norr' 3.0; Send 'rr on' 5.0
  }
  foreach ($key in 'ssr', 'windtrees', 'wind', 'npc', 'objblur', 'reflex') {
    if ($state[$key] -ne 'on') { continue }
    Send "$key off" 3.0; LogFrames "perf_no$key"; Send "$key on" 2.0
  }
  Send 'scale 0.33' 6.0; Send 'scale'; LogFrames 'perf_scale33'; Send 'gpuprofile 60 perf_scale33' 3.0
  Send "scale $scale" 6.0; Send 'dlss off' 3.0; LogFrames 'perf_taa'
  Send 'dlss on' 3.0; Send "scale $scale" 6.0; Send 'scale'; LogFrames 'perf_base2'
  foreach ($key in $state.Keys) { Send "$key $($state[$key])" }
  if ($fg -and $fg -ne '0') { Send "fg $fg" }
} elseif ($Round -eq 'fg') {
  # Frame generation at the current spot: a 5 s frame-time log each with it off, 2x, 3x and 4x, and DLSS-G's state
  # (frames shown per rendered frame, status, inputs). Summarize with tools\frames.py shots\fg_*.csv. Restores the
  # ini's setting.
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $fg = ($ini | Where-Object { $_ -match '^FrameGeneration=' }) -replace '^FrameGeneration=', ''
  if (-not $fg) { $fg = '0' }
  foreach ($mode in '0', '2', '3', '4') { Send "fg $mode" 4.0; Send 'fg'; LogFrames "fg_$mode" }
  Send "fg $fg"
} elseif ($Round -eq 'mvcost') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $wind = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  $trees = if ($ini -match '^WindTrees=0') { 'off' } else { 'on' }
  $npc = if ($ini -match '^NpcMotion=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'wind on'; Send 'windtrees on'; Send 'npc on' 3.0; LogFrames 'mv_all'
  Send 'windtrees off' 2.0; LogFrames 'mv_notrees'
  Send 'wind off' 2.0; LogFrames 'mv_nowind'
  Send 'npc off' 2.0; LogFrames 'mv_none'
  Send 'wind on'; Send 'windtrees on'; Send 'npc on' 3.0; LogFrames 'mv_all2'
  Send "windtrees $trees"; Send "wind $wind"; Send "npc $npc"
} elseif ($Round -eq 'rtfoliage') {
  # The cost of alpha-tested foliage in our rays: a 30-frame GPU profile and a screenshot per setting (the game's;
  # AO, shadow or both rays without the alpha test; AO rays at 1/2 and 1/4 of the game's length; shadows opaque
  # beyond half the game's distance). Summarize with gpu_profile.py shots\gpuprofile_rf_*.csv. Restores the defaults.
  $game = (Send 'rtsamples') -join ' '
  $aoLength = [double]([regex]::Match($game, 'AO length ([\d.]+) m').Groups[1].Value)
  $shadowFar = [double]([regex]::Match($game, 'beyond ([\d.]+) m').Groups[1].Value)
  $rounds = [ordered]@{
    base = @('rtalpha on on'); aoopaque = @('rtalpha off on'); shadowopaque = @('rtalpha on off')
    bothopaque = @('rtalpha off off')
    aohalf = @('rtalpha on on', ('rtaolen ' + ($aoLength / 2).ToString([cultureinfo]::InvariantCulture)))
    aoquarter = @(('rtaolen ' + ($aoLength / 4).ToString([cultureinfo]::InvariantCulture)))
    shadowfarhalf = @('rtaolen 0', ('rtshadowfar ' + ($shadowFar / 2).ToString([cultureinfo]::InvariantCulture)))
    base2 = @('rtshadowfar 0')
  }
  foreach ($name in $rounds.Keys) {
    foreach ($command in $rounds[$name]) { Send $command | Out-Null }
    Start-Sleep -Seconds 1
    Send "gpuprofile 30 rf_$name"; Send "capture rf_$name"
  }
  Send 'rtalpha game game'; Send 'rtaolen 0'; Send 'rtshadowfar 0'
} elseif ($Round -eq 'water') {
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WaterReflections=0') { 'off' } else { 'on' }
  New-Item -ItemType Directory -Force (Join-Path $Folder 'shots\water') | Out-Null
  function Burst([string]$Name) { foreach ($i in 0..7) { Send "capture water/${Name}_$i" | Out-Null }; "captured $Name" }
  Send 'dlss on'; Send 'scale'; Send 'ssr on' 1.5; Send 'ssr'; Burst 'on'
  Send 'ssr off' 1.5; Send 'ssr'; Burst 'off'; LogFrames 'water_off'; Send 'gpuprofile 30 water_off' 2.0
  Send 'ssr on' 1.5; Send 'ssr'; LogFrames 'water_on'; Send 'gpuprofile 30 water_on' 2.0; Burst 'on2'
  Send 'scale 1' 5.0; Send 'dlss off' 3.0; Send 'scale'; Burst 'taa'
  Send 'dlss on'; Send 'scale 0.5' 5.0; Send "ssr $restore"; Send 'scale'; Send 'ssr'
} elseif ($Round -eq 'treebatch') {
  # Tree wind patches before their command lists (treebatch on) against inline in the game's lists (off), and trees
  # off: per mode the wind stats, an 8-frame motion dump (the vegetation's motion should match between the modes), a
  # 5 s frame-time log and a 60-frame GPU profile. Frame generation off meanwhile. Summarize with tools\frames.py
  # shots\tb_*.csv and tools\gpu_profile.py shots\gpuprofile_tb_on.csv --compare shots\gpuprofile_tb_off.csv.
  # Restores the ini's settings.
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $trees = if ($ini -match '^WindTrees=0') { 'off' } else { 'on' }
  $fg = ($ini | Where-Object { $_ -match '^FrameGeneration=' }) -replace '^FrameGeneration=', ''
  if ($fg -and $fg -ne '0') { Send 'fg 0' 2.0 }
  Send 'dlss on'; Send 'windtrees on' 3.0
  foreach ($pass in @(('on', 'tb_on'), ('off', 'tb_off'), ('on', 'tb_on2'))) {
    $mode, $name = $pass
    Send "treebatch $mode" 3.0; Send 'windstats' 1.0; Send 'windstats'
    Send "mv 8 $name" 1.0; LogFrames $name; Send "gpuprofile 60 $name" 3.0
  }
  Send 'windtrees off' 3.0; LogFrames 'tb_notrees'; Send 'gpuprofile 60 tb_notrees' 3.0
  Send 'treebatch on'; Send "windtrees $trees"
  if ($fg -and $fg -ne '0') { Send "fg $fg" }
} elseif ($Round -eq 'treecheck') {
  # Whether the tree wind patches that run before their command lists are still right at the draws: "treebatch
  # verify" checks every patch at its draw (windstats: copies and entries that differ and trees not in the table
  # should all be 0). Then "treebatch alternate" places the patches before their lists in odd frames and inline in
  # even ones for two 16-frame motion dumps (tools\tree_alternate.py shots tc_alt1 tc_alt2: the canopy motion of both
  # placements in neighboring frames). Frame generation off meanwhile. Restores the ini's settings.
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $trees = if ($ini -match '^WindTrees=0') { 'off' } else { 'on' }
  $fg = ($ini | Where-Object { $_ -match '^FrameGeneration=' }) -replace '^FrameGeneration=', ''
  if ($fg -and $fg -ne '0') { Send 'fg 0' 2.0 }
  Send 'dlss on'; Send 'windtrees on' 3.0
  Send 'treebatch verify' 2.0; Send 'windstats' 3.0; Send 'windstats' 3.0; Send 'windstats'
  Send 'treebatch alternate' 2.0; Send 'windstats' 1.0; Send 'mv 16 tc_alt1' 1.0; Send 'mv 16 tc_alt2' 1.0
  Send 'treebatch on' 2.0; Send 'windstats' 1.0; Send 'windstats'
  Send "windtrees $trees"
  if ($fg -and $fg -ne '0') { Send "fg $fg" }
} elseif ($Round -eq 'treecost') {
  # Where the trees' cost goes: grass and trees with and without the resolve draw, grass only.
  Send 'dlss on'; Send 'wind on'; Send 'windtrees on' 3.0; LogFrames 'tc_trees'
  Send 'windresolve off' 1.0; LogFrames 'tc_noresolve'; Send 'windresolve on'
  Send 'windtrees off' 1.0; LogFrames 'tc_grass'
  Send 'windtrees on' 2.0; LogFrames 'tc_trees2'
} elseif ($Round -eq 'treedump') {
  # SpeedTree wind of 8 consecutive frames (tools/tree_check.py).
  Send 'treedump 8'
} elseif ($Round -eq 'survey') {
  # What still moves without motion vectors with the grass wind motion on: two 8-frame dumps a second apart
  # (mv_check.py --compare), the grass pass's coverage marks and a screenshot.
  $ini = Get-Content (Join-Path $Folder 'EldenRingDLSS.ini') -ErrorAction SilentlyContinue
  $restore = if ($ini -match '^WindMotion=1') { 'on' } else { 'off' }
  Send 'dlss on'; Send 'wind on' 3.0; Send 'windstats' 1.0; Send 'windstats'
  Send 'mv 8 surv_a' 1.0; Send 'mv 8 surv_b'
  Send 'windmerge merged' 0.5; Send 'mv 1 surv_merged'; Send 'windmerge on' 0.5
  Send 'capture surv_shot'
  Send "wind $restore"
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
