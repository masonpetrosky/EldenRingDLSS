# Builds EldenRingDLSS.addon64 with the newest installed Visual Studio (x64, Release).
param([string]$Config = 'Release', [switch]$Tests)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path "$PSScriptRoot\third_party\dlss\lib\nvsdk_ngx_s.lib")) {
  & "$PSScriptRoot\tools\fetch_dlss_sdk.ps1" | Out-Null
}
if (-not (Test-Path "$PSScriptRoot\third_party\nvapi\amd64\nvapi64.lib")) {
  & "$PSScriptRoot\tools\fetch_nvapi.ps1" | Out-Null
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x64 toolset was not found.' }

$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$devShell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

Push-Location $PSScriptRoot
try {
  $buildTests = if ($Tests) { 'ON' } else { 'OFF' }
  cmake -S . -B build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DERDLSS_BUILD_TESTS=$buildTests"
  if ($LASTEXITCODE) { throw 'CMake configure failed.' }
  cmake --build build
  if ($LASTEXITCODE) { throw 'Build failed.' }
  if ($Tests) {
    ctest --test-dir build --output-on-failure
    if ($LASTEXITCODE) { throw 'Regression checks failed.' }
  }
  Get-Item build\EldenRingDLSS.addon64
} finally {
  Pop-Location
}
