param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$PayloadDir,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [string]$VcRedistPath = ""
)

$ErrorActionPreference = "Stop"

function Require-Command {
    param([string]$Name)

    if (Get-Command $Name -ErrorAction SilentlyContinue) {
        return
    }

    if ($Name -eq "wix") {
        Write-Host "Installing WiX v4 CLI via dotnet tool..."
        dotnet tool install --global wix --version 4.* | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "dotnet tool install wix failed with exit code $LASTEXITCODE."
        }
        if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
            throw "Failed to install wix CLI."
        }
        return
    }

    throw "Required command '$Name' was not found."
}

function Ensure-Directory {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -ItemType Directory -Force | Out-Null
    }
}

Require-Command -Name "wix"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\.." )).Path
$payloadDirResolved = (Resolve-Path $PayloadDir).Path
Ensure-Directory -Path $OutputDir
$outputDirResolved = (Resolve-Path $OutputDir).Path

if ([string]::IsNullOrWhiteSpace($VcRedistPath)) {
    $VcRedistPath = Join-Path $outputDirResolved "vc_redist.x64.exe"
}

if (-not (Test-Path -LiteralPath $VcRedistPath)) {
    Write-Host "Downloading VC++ redistributable x64..."
    Invoke-WebRequest -Uri "https://aka.ms/vs/17/release/vc_redist.x64.exe" -OutFile $VcRedistPath
}

$wixProduct = Join-Path $PSScriptRoot "Product.wxs"
$wixBundle = Join-Path $PSScriptRoot "Bundle.wxs"

$msiPath = Join-Path $outputDirResolved "RipperForge-$Version-x64.msi"
$bundlePath = Join-Path $outputDirResolved "RipperForge-Setup-x64.exe"

wix extension add -g WixToolset.Bal.wixext/4.0.6 | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "wix extension add failed with exit code $LASTEXITCODE."
}
wix extension add -g WixToolset.Util.wixext/4.0.6 | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "wix extension add failed with exit code $LASTEXITCODE."
}

wix build $wixProduct `
    -arch x64 `
    -d ProductVersion=$Version `
    -d PayloadDir=$payloadDirResolved `
    -o $msiPath
if ($LASTEXITCODE -ne 0) {
    throw "wix build (MSI) failed with exit code $LASTEXITCODE."
}

wix build $wixBundle `
    -arch x64 `
    -ext WixToolset.Bal.wixext/4.0.6 `
    -ext WixToolset.Util.wixext/4.0.6 `
    -d ProductVersion=$Version `
    -d MsiPath=$msiPath `
    -d VCRedistPath=$VcRedistPath `
    -o $bundlePath
if ($LASTEXITCODE -ne 0) {
    throw "wix build (bundle) failed with exit code $LASTEXITCODE."
}

Write-Host "MSI:    $msiPath"
Write-Host "Bundle: $bundlePath"
