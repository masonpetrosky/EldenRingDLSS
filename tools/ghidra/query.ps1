# Runs one ErQuery.java command against the analyzed Ghidra project and prints the result.
#   pwsh tools/ghidra/query.ps1 decompile 141A340F0
#   pwsh tools/ghidra/query.ps1 xrefs 143D61F98
param([Parameter(Mandatory, Position = 0)][string]$Command, [Parameter(ValueFromRemainingArguments)][string[]]$Args)
$ErrorActionPreference = 'Stop'
# Ghidra, its JDK and the project live in the shared tools folder (Documents\Tools\Ghidra).
$tools = Join-Path $env:USERPROFILE 'Documents\Tools\Ghidra'
$project = Join-Path $tools 'projects'
$env:JAVA_HOME = (Get-ChildItem $tools -Directory -Filter 'jdk-*' | Select-Object -First 1).FullName
$env:GHIDRA_HEADLESS_MAXMEM = '12G'
$out = Join-Path $project ("q_{0}.txt" -f (Get-Date -Format 'HHmmss'))
$ghidra = (Get-ChildItem $tools -Directory -Filter 'ghidra_*' | Select-Object -First 1).FullName
& "$ghidra\support\analyzeHeadless.bat" $project EldenRing -process eldenring.exe -noanalysis -readOnly `
  -scriptPath $PSScriptRoot -postScript ErQuery.java $out $Command @Args *> (Join-Path $project 'q_run.txt')
if (-not (Test-Path $out)) { Get-Content (Join-Path $project 'q_run.txt') -Tail 20; throw 'query failed' }
Get-Content $out
