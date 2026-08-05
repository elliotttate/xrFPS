# Installs FlipFPS as a per-user implicit OpenXR API layer (no admin needed).
# Copies the built DLL + manifest to %LOCALAPPDATA%\FlipFPS and registers it
# under HKCU. Every OpenXR app started afterwards loads the layer.
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\Release')
)

$ErrorActionPreference = 'Stop'

$dll = Join-Path $BuildDir 'XrApiLayer_flipfps.dll'
$json = Join-Path $BuildDir 'XrApiLayer_flipfps.json'
if (-not (Test-Path $dll)) { throw "Built layer not found: $dll (build first, or pass -BuildDir)" }
if (-not (Test-Path $json)) { throw "Manifest not found next to the DLL: $json" }

$dest = Join-Path $env:LOCALAPPDATA 'FlipFPS'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item $dll, $json -Destination $dest -Force

$key = 'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
$manifestPath = Join-Path $dest 'XrApiLayer_flipfps.json'
New-ItemProperty -Path $key -Name $manifestPath -Value 0 -PropertyType DWord -Force | Out-Null

Write-Output "FlipFPS installed: $manifestPath"
Write-Output "Disable temporarily by setting the environment variable FLIPFPS_DISABLE=1"
