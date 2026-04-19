param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"

function Ensure-Directory {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Find-MSBuild {
    $candidates = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "MSBuild.exe not found. Install Visual Studio 2022 C++ Build Tools (v143)."
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot ".." )).Path
$releaseRoot = Join-Path $repoRoot ("artifacts\release\{0}" -f $Version)

$guardRoot = Join-Path $repoRoot "artifacts\release"
if (Test-Path -LiteralPath $releaseRoot) {
    $fullReleaseRoot = [System.IO.Path]::GetFullPath($releaseRoot)
    $fullGuardRoot = [System.IO.Path]::GetFullPath($guardRoot)
    if (-not $fullReleaseRoot.StartsWith($fullGuardRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete path outside artifacts/release: $fullReleaseRoot"
    }
    Remove-Item -LiteralPath $releaseRoot -Recurse -Force
}

$stageAppDir = Join-Path $releaseRoot "stage\app"
$installerDir = Join-Path $releaseRoot "installer"
$portableDir = Join-Path $releaseRoot "portable"
$notesDir = Join-Path $releaseRoot "notes"

Ensure-Directory -Path $stageAppDir
Ensure-Directory -Path (Join-Path $stageAppDir "capture")
Ensure-Directory -Path (Join-Path $stageAppDir "plugins")
Ensure-Directory -Path $installerDir
Ensure-Directory -Path $portableDir
Ensure-Directory -Path $notesDir

$msbuild = Find-MSBuild
$solutionPath = Join-Path $repoRoot "RipperForge.sln"

& $msbuild $solutionPath /t:Build /m "/p:Configuration=$Configuration" "/p:Platform=$Platform"

$buildOutDir = Join-Path $repoRoot ("build\{0}" -f $Configuration)
$exePath = Join-Path $buildOutDir "RipperForge.exe"
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Release binary not found: $exePath"
}

Copy-Item -LiteralPath $exePath -Destination (Join-Path $stageAppDir "RipperForge.exe") -Force

$pdbPath = Join-Path $buildOutDir "RipperForge.pdb"
if (Test-Path -LiteralPath $pdbPath) {
    Copy-Item -LiteralPath $pdbPath -Destination (Join-Path $stageAppDir "RipperForge.pdb") -Force
}

$readmePath = Join-Path $repoRoot "README.md"
if (Test-Path -LiteralPath $readmePath) {
    Copy-Item -LiteralPath $readmePath -Destination (Join-Path $stageAppDir "README.md") -Force
}

$captureCandidates = @(
    (Join-Path $repoRoot "capture\ripper_new6.dll"),
    (Join-Path $repoRoot "external\AssetRIpper\ripper_new6.dll"),
    (Join-Path $repoRoot "ripper_new6.dll")
)
$captureSource = $captureCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $captureSource) {
    throw "Could not locate ripper_new6.dll. Checked: $($captureCandidates -join ', ')"
}
Copy-Item -LiteralPath $captureSource -Destination (Join-Path $stageAppDir "capture\ripper_new6.dll") -Force

$pluginSourceDir = Join-Path $repoRoot "plugins"
if (Test-Path -LiteralPath $pluginSourceDir) {
    Get-ChildItem -LiteralPath $pluginSourceDir -Filter *.dll -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stageAppDir "plugins\$($_.Name)") -Force
    }
}

$installerExe = Join-Path $installerDir "RipperForge-Setup-x64.exe"
$installerMsi = Join-Path $installerDir ("RipperForge-{0}-x64.msi" -f $Version)

if (-not $SkipInstaller) {
    $installerScript = Join-Path $repoRoot "installer\wix\build-installer.ps1"
    & $installerScript -Version $Version -PayloadDir $stageAppDir -OutputDir $installerDir
}

$portableZip = Join-Path $portableDir ("RipperForge-portable-{0}-x64.zip" -f $Version)
Compress-Archive -Path (Join-Path $stageAppDir "*") -DestinationPath $portableZip -CompressionLevel Optimal -Force

$checksumFile = Join-Path $releaseRoot "SHA256SUMS.txt"
$artifacts = @()
if (Test-Path -LiteralPath $installerExe) { $artifacts += $installerExe }
if (Test-Path -LiteralPath $installerMsi) { $artifacts += $installerMsi }
if (Test-Path -LiteralPath $portableZip) { $artifacts += $portableZip }

$checksumLines = foreach ($artifact in $artifacts) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifact).Hash.ToLowerInvariant()
    "{0} *{1}" -f $hash, ([System.IO.Path]::GetFileName($artifact))
}
$checksumLines | Set-Content -Path $checksumFile -Encoding ascii

$notesFile = Join-Path $notesDir "RELEASE_NOTES_TEMPLATE.md"
@"
# RipperForge v$Version

## Highlights
- ImGui docking UI with reverse toolkit workflows.
- LocalAppData runtime-data migration for installed builds.
- Dual plugin loading (Program Files + user plugin folder).
- WiX/Burn installer bundles VC++ x64 redistributable.

## Artifacts
- RipperForge-Setup-x64.exe
- RipperForge-$Version-x64.msi
- RipperForge-portable-$Version-x64.zip
- SHA256SUMS.txt

## Install Notes
- Setup installs app binaries to Program Files.
- Runtime writable data is stored under `%LocalAppData%\\RipperForge`.
- Uninstall removes binaries and shortcuts; user data is preserved.
"@ | Set-Content -Path $notesFile -Encoding ascii

$latestRoot = Join-Path $guardRoot "latest"
if (Test-Path -LiteralPath $latestRoot) {
    $fullLatestRoot = [System.IO.Path]::GetFullPath($latestRoot)
    if (-not $fullLatestRoot.StartsWith($fullGuardRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete path outside artifacts/release: $fullLatestRoot"
    }
    Remove-Item -LiteralPath $latestRoot -Recurse -Force
}

$latestInstallerDir = Join-Path $latestRoot "installer"
$latestPortableDir = Join-Path $latestRoot "portable"
$latestNotesDir = Join-Path $latestRoot "notes"
Ensure-Directory -Path $latestInstallerDir
Ensure-Directory -Path $latestPortableDir
Ensure-Directory -Path $latestNotesDir

if (Test-Path -LiteralPath $installerExe) {
    Copy-Item -LiteralPath $installerExe -Destination (Join-Path $latestInstallerDir "RipperForge-Setup-x64.exe") -Force
}
if (Test-Path -LiteralPath $installerMsi) {
    Copy-Item -LiteralPath $installerMsi -Destination (Join-Path $latestInstallerDir ([System.IO.Path]::GetFileName($installerMsi))) -Force
}
if (Test-Path -LiteralPath $portableZip) {
    Copy-Item -LiteralPath $portableZip -Destination (Join-Path $latestPortableDir ([System.IO.Path]::GetFileName($portableZip))) -Force
}
if (Test-Path -LiteralPath $checksumFile) {
    Copy-Item -LiteralPath $checksumFile -Destination (Join-Path $latestRoot "SHA256SUMS.txt") -Force
}
if (Test-Path -LiteralPath $notesFile) {
    Copy-Item -LiteralPath $notesFile -Destination (Join-Path $latestNotesDir "RELEASE_NOTES_TEMPLATE.md") -Force
}

Write-Host "Release root: $releaseRoot"
Write-Host "Stage app:    $stageAppDir"
Write-Host "Installer:    $installerExe"
Write-Host "Portable zip: $portableZip"
Write-Host "Checksums:    $checksumFile"
Write-Host "Notes:        $notesFile"
Write-Host "Latest alias: $latestRoot"
