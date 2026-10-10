# Sends one command to the dev build's remote control (remote.txt in the dev folder) and prints its answer.
#   pwsh tools/remote.ps1 "drsforce primary 0.67"
param(
  [Parameter(Mandatory)][string]$Command,
  [string]$Folder = "$env:LOCALAPPDATA\garyttierney\me3\config\profiles\natives\EldenRingDLSS-dev"
)
$ErrorActionPreference = 'Stop'
$done = Join-Path $Folder 'remote.done'
Remove-Item $done -ErrorAction SilentlyContinue
Set-Content -Path (Join-Path $Folder 'remote.txt') -Value $Command -Encoding ascii
for ($i = 0; $i -lt 300 -and -not (Test-Path $done); $i++) { Start-Sleep -Milliseconds 100 }
if (-not (Test-Path $done)) { throw "No response to '$Command' (is the game running with the dev build?)" }
Get-Content $done
