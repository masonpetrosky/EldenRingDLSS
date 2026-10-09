# Captures the same spot with several anti-aliasing settings through the add-on's remote control.
# Stand still in game first. Writes shots to <add-on folder>\shots and restores DLAA without sharpening.
param([string]$Folder = "$env:LOCALAPPDATA\garyttierney\me3\config\profiles\natives\EldenRingDLSS-dev")
$ErrorActionPreference = 'Stop'

function Send([string]$Command, [double]$Settle = 0) {
  $done = Join-Path $Folder 'remote.done'
  Remove-Item $done -ErrorAction SilentlyContinue
  Set-Content -Path (Join-Path $Folder 'remote.txt') -Value $Command -Encoding ascii
  for ($i = 0; $i -lt 100 -and -not (Test-Path $done); $i++) { Start-Sleep -Milliseconds 100 }
  if (-not (Test-Path $done)) { throw "No response to '$Command' (is the game running with the dev build?)" }
  Get-Content $done
  if ($Settle -gt 0) { Start-Sleep -Milliseconds ([int]($Settle * 1000)) }
}

Send 'dlss on'; Send 'sharpen off' 1.5; Send 'capture dlaa'
Send 'sharpen on'; Send 'strength 0.25' 0.5; Send 'capture dlaa_sharp25'
Send 'strength 0.10' 0.5; Send 'capture dlaa_sharp10'
Send 'sharpen off'; Send 'preset 10' 2.5; Send 'capture dlaa_presetJ'
Send 'preset 0'; Send 'dlss off' 2.0; Send 'capture taa'
Send 'dlss on' 1.0
