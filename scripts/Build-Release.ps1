# Builds every xrFPS component, stages the payload, and produces the release
# artifacts: an installer, a portable zip, and a SHA256SUMS.txt whose format the
# in-app updater parses.
[CmdletBinding()]
param(
    [string]$Version = '0.2.0',
    [string]$FileVersion = '0.2.0.0',
    [string]$OpenXrSdkPath = 'C:\Users\ellio\OneDrive\Documents\GitHub\OpenXR-SDK'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Version '$Version' is not <major>.<minor>.<patch>[-prerelease]."
}
if ($FileVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw "FileVersion '$FileVersion' must be four numeric parts."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$artifactsRoot = Join-Path $repoRoot 'artifacts'
$releaseDir = Join-Path $artifactsRoot "v$Version"
$stageDir = Join-Path $artifactsRoot ".stage-v$Version"
$payloadRoot = Join-Path $stageDir 'xrFPS'

# Guard: never delete anything outside artifacts\.
foreach ($target in @($releaseDir, $stageDir)) {
    $full = [IO.Path]::GetFullPath($target)
    if (-not $full.StartsWith([IO.Path]::GetFullPath($artifactsRoot), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside the artifacts directory: $full"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
New-Item -ItemType Directory -Force -Path $releaseDir, $payloadRoot | Out-Null

Write-Host "[1/6] Building native targets"
$buildDir = Join-Path $repoRoot 'build'
$cmakeArgs = @('-S', $repoRoot, '-B', $buildDir, '-A', 'x64')
if ($OpenXrSdkPath -and (Test-Path -LiteralPath $OpenXrSdkPath)) {
    $cmakeArgs += "-DOPENXR_SDK_PATH=$OpenXrSdkPath"
}
& cmake @cmakeArgs | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildDir --config Release --target XrApiLayer_xrfps xrfps_core | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }

Write-Host "[2/6] Publishing the Control Center"
$appPublish = Join-Path $stageDir 'app-publish'
& dotnet publish (Join-Path $repoRoot 'app') -c Release -p:Platform=x64 `
    -p:Version=$Version -p:AssemblyVersion=$FileVersion -p:FileVersion=$FileVersion `
    -p:InformationalVersion=$Version -o $appPublish | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Control Center publish failed.' }

# The app compares its own version against GitHub releases, so a mis-stamped
# build would offer an update it already has - forever.
$exe = Join-Path $appPublish 'xrFPS.ControlCenter.exe'
$stamped = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
if ($stamped -notlike "$Version*") {
    throw "Control Center reports version '$stamped' but this build is '$Version'."
}

Write-Host "[3/6] Staging the payload"
Copy-Item -Path (Join-Path $appPublish '*') -Destination $payloadRoot -Recurse -Force
foreach ($file in @('XrApiLayer_xrfps.dll', 'xrfps_core.dll')) {
    Copy-Item -LiteralPath (Join-Path $buildDir "Release\$file") -Destination $payloadRoot -Force
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'manifest\XrApiLayer_xrfps.json') -Destination $payloadRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination $payloadRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $payloadRoot -Force

Write-Host "[4/6] Compiling the installer"
$iscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
if (-not (Test-Path -LiteralPath $iscc)) {
    # Fall back to the registered install location before giving up.
    $key = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1'
    $location = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).InstallLocation
    if ($location) { $iscc = Join-Path $location 'ISCC.exe' }
}
if (-not (Test-Path -LiteralPath $iscc)) { throw "Inno Setup 6 compiler not found: $iscc" }

& $iscc '/Qp' "/DMyAppVersion=$Version" "/DMyFileVersion=$FileVersion" `
    "/DPayloadRoot=$payloadRoot" "/DOutputDirectory=$releaseDir" `
    (Join-Path $repoRoot 'installer\xrFPS.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }

Write-Host "[5/6] Packing the portable zip"
$zip = Join-Path $releaseDir "xrFPS-$Version-Portable.zip"
Compress-Archive -Path $payloadRoot -DestinationPath $zip -CompressionLevel Optimal -Force

Write-Host "[6/6] Writing checksums"
# "<hash> *<name>", the format AppUpdateService parses to verify a download.
$checksumPath = Join-Path $releaseDir 'SHA256SUMS.txt'
$lines = Get-ChildItem -LiteralPath $releaseDir -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object Name |
    ForEach-Object { "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()) *$($_.Name)" }
$lines | Set-Content -LiteralPath $checksumPath -Encoding ascii

Remove-Item -LiteralPath $stageDir -Recurse -Force

[pscustomobject]@{
    Version   = $Version
    Directory = $releaseDir
    Installer = (Get-ChildItem -LiteralPath $releaseDir -Filter '*-Setup.exe' | Select-Object -First 1).Name
    Portable  = Split-Path -Leaf $zip
    Checksums = 'SHA256SUMS.txt'
}
