# Runs one ErQuery.java command against the analyzed Ghidra project and prints the result.
#   pwsh tools/ghidra/query.ps1 decompile 141A340F0
#   pwsh tools/ghidra/query.ps1 xrefs 143D61F98
param([Parameter(Mandatory, Position = 0)][string]$Command, [Parameter(ValueFromRemainingArguments)][string[]]$Args)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$tools = Join-Path $root '.tools'
$project = Join-Path $env:USERPROFILE 'Documents\GhidraProjects'
$env:JAVA_HOME = (Get-ChildItem (Join-Path $tools 'jdk') -Directory | Select-Object -First 1).FullName
$env:GHIDRA_HEADLESS_MAXMEM = '12G'
$out = Join-Path $project ("q_{0}.txt" -f (Get-Date -Format 'HHmmss'))
$ghidra = (Get-ChildItem (Join-Path $tools 'ghidra') -Directory | Select-Object -First 1).FullName
& "$ghidra\support\analyzeHeadless.bat" $project EldenRing -process eldenring.exe -noanalysis -readOnly `
  -scriptPath $PSScriptRoot -postScript ErQuery.java $out $Command @Args *> (Join-Path $project 'q_run.txt')
if (-not (Test-Path $out)) { Get-Content (Join-Path $project 'q_run.txt') -Tail 20; throw 'query failed' }
Get-Content $out
