<#
.SYNOPSIS
    Builds AppAssist.exe (MFC GUI) and AppAssistCli.exe into build\bin\<Config>.

.DESCRIPTION
    Fetches dependencies if needed (scripts\fetch_deps.ps1), then configures and builds with the
    CMake that ships with Visual Studio 2022 (no separate CMake install needed).

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Version
    Version stamped into the programs (major.minor.patch). Default: the one already configured
    (1.0.0 on a fresh build folder). The release workflow passes the next tag's version.

.PARAMETER BuildNumber
    Fourth part of the file version, e.g. the CI run number. Default: 0.
#>
param(
    [ValidateSet("Release", "Debug")][string]$Config = "Release",
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [ValidateRange(0, 65535)][int]$BuildNumber = 0
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
Set-Location $Root

& (Join-Path $PSScriptRoot "fetch_deps.ps1")

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.ATLMFC -property installationPath | Select-Object -First 1
    if (-not $vs) { throw "Visual Studio 2022 with the 'C++ MFC for latest build tools' component is required." }
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    # CMake and MSBuild write progress to stderr; Windows PowerShell would treat that as a terminating
    # error under "Stop", so judge success by the exit code instead.
    $ErrorActionPreference = "Continue"
    & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

$configure = @("-S", ".", "-B", "build", "-G", "Visual Studio 17 2022", "-A", "x64", "-DAPPASSIST_BUILD_NUMBER=$BuildNumber")
if ($Version) { $configure += "-DAPPASSIST_VERSION=$Version" }
Invoke-Checked $cmake $configure
Invoke-Checked $cmake @("--build", "build", "--config", $Config, "--parallel", "--", "/v:minimal", "/nologo")

Get-ChildItem (Join-Path $Root "build\bin\$Config") -Filter *.exe | ForEach-Object {
    Write-Host ("Built {0} ({1:N1} MB)" -f $_.FullName, ($_.Length / 1MB)) -ForegroundColor Green
}
