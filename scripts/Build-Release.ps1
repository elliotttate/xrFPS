# Builds every xrFPS component, stages the payload, and produces the release
# artifacts: an installer, a portable zip, and a SHA256SUMS.txt whose format the
# in-app updater parses.
[CmdletBinding()]
param(
    [string]$Version = '0.5.1',
    [string]$FileVersion = '0.5.1.0',
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

Write-Host "[1/7] Building native targets"
$buildDir = Join-Path $repoRoot 'build'
$cmakeArgs = @('-S', $repoRoot, '-B', $buildDir, '-A', 'x64')
if ($OpenXrSdkPath -and (Test-Path -LiteralPath $OpenXrSdkPath)) {
    $cmakeArgs += "-DOPENXR_SDK_PATH=$OpenXrSdkPath"
}
& cmake @cmakeArgs | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildDir --config Release --target XrApiLayer_xrfps xrfps_core | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }

Write-Host "[2/7] Publishing the Control Center"
$appPublish = Join-Path $stageDir 'app-publish'
# The runtime packs for a self-contained publish restore from nuget.org; a
# machine with no configured NuGet sources fails with NU1101 without this.
& dotnet publish (Join-Path $repoRoot 'app') -c Release -p:Platform=x64 `
    -p:Version=$Version -p:AssemblyVersion=$FileVersion -p:FileVersion=$FileVersion `
    -p:InformationalVersion=$Version -o $appPublish `
    --source https://api.nuget.org/v3/index.json | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Control Center publish failed.' }

# The app compares its own version against GitHub releases, so a mis-stamped
# build would offer an update it already has - forever.
$exe = Join-Path $appPublish 'xrFPS.ControlCenter.exe'
$stamped = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
if ($stamped -notlike "$Version*") {
    throw "Control Center reports version '$stamped' but this build is '$Version'."
}

# v0.4.1 shipped framework-dependent and would not start without a hand-
# installed .NET 9 runtime. Refuse to ever produce that build again.
foreach ($hostFile in @('hostfxr.dll', 'coreclr.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $appPublish $hostFile))) {
        throw "Publish output lacks $hostFile - the app is not self-contained."
    }
}

Write-Host "[3/7] Staging the payload"
Copy-Item -Path (Join-Path $appPublish '*') -Destination $payloadRoot -Recurse -Force
foreach ($file in @('XrApiLayer_xrfps.dll', 'xrfps_core.dll')) {
    Copy-Item -LiteralPath (Join-Path $buildDir "Release\$file") -Destination $payloadRoot -Force
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'manifest\XrApiLayer_xrfps.json') -Destination $payloadRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination $payloadRoot -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $payloadRoot -Force

Write-Host "[4/7] Bundling the VC++ runtime"
# The self-contained Windows App SDK and xrfps_core.dll (dynamic CRT) both
# need the VC++ runtime, which a clean Windows install lacks. App-local copies
# keep the installer and the portable zip free of prerequisites.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found: $vswhere" }
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'No Visual Studio with the VC++ toolset found.' }
$crtDir = Get-ChildItem -Path (Join-Path $vsRoot 'VC\Redist\MSVC') -Directory |
    Where-Object { $_.Name -match '^\d' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Get-ChildItem -Path (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $crtDir) { throw "No x64 CRT redist directory found under $vsRoot\VC\Redist\MSVC" }
Copy-Item -Path (Join-Path $crtDir.FullName '*.dll') -Destination $payloadRoot -Force
foreach ($dll in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot $dll))) {
        throw "VC++ runtime staging missed $dll."
    }
}

Write-Host "[5/7] Compiling the installer"
# Inno Setup installs per-machine or per-user; check both, then the registry.
$isccCandidates = @(
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
)
foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1',
                   'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1')) {
    $entry = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
    # StrictMode: the property may be absent even when the key exists.
    if ($entry -and $entry.PSObject.Properties['InstallLocation'] -and $entry.InstallLocation) {
        $isccCandidates += (Join-Path $entry.InstallLocation 'ISCC.exe')
    }
}
$iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 compiler not found; install Inno Setup 6 (a per-user install works) and re-run.' }

& $iscc '/Qp' "/DMyAppVersion=$Version" "/DMyFileVersion=$FileVersion" `
    "/DPayloadRoot=$payloadRoot" "/DOutputDirectory=$releaseDir" `
    (Join-Path $repoRoot 'installer\xrFPS.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }

Write-Host "[6/7] Packing the portable zip"
$zip = Join-Path $releaseDir "xrFPS-$Version-Portable.zip"
Compress-Archive -Path $payloadRoot -DestinationPath $zip -CompressionLevel Optimal -Force

Write-Host "[7/7] Writing checksums"
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
