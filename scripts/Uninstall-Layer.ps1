# Removes the FlipFPS implicit layer registration and installed files.
$ErrorActionPreference = 'Stop'

$dest = Join-Path $env:LOCALAPPDATA 'FlipFPS'
$manifestPath = Join-Path $dest 'XrApiLayer_flipfps.json'
$key = 'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'

if (Test-Path $key) {
    Remove-ItemProperty -Path $key -Name $manifestPath -ErrorAction SilentlyContinue
}
if (Test-Path $dest) {
    Remove-Item -Recurse -Force $dest
}
Write-Output 'FlipFPS uninstalled.'
