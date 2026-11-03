<#
.SYNOPSIS
    Quick check of a built AppAssistCli.exe, without a model: version, indexing and a diagnosis.

.DESCRIPTION
    Indexes testdata\java-inventory, analyzes testdata\java-failure.log and checks that the report
    finds the failing setting (page.size, value "fifty" in inventory.properties). Used by the release
    workflow before a build is packaged.

.PARAMETER Exe
    The CLI to test. Default: build\bin\Release\AppAssistCli.exe.

.PARAMETER Version
    If given, AppAssistCli --version must report this major.minor.patch.
#>
param(
    [string]$Exe = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\bin\Release\AppAssistCli.exe"),
    [string]$Version
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("appassist-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null

function Invoke-Cli {
    # The CLI writes progress to stderr; judge it by the exit code, not by stderr output.
    $ErrorActionPreference = "Continue"
    $output = & $Exe @args 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "AppAssistCli $($args -join ' ') failed with exit code $LASTEXITCODE`n$($output -join "`n")" }
    $output
}

try {
    $reported = (Invoke-Cli --version) -join ""
    Write-Host $reported
    if ($Version -and $reported -notmatch "^AppAssistCli $([regex]::Escape($Version))\.\d+$") {
        throw "expected version $Version, got '$reported'"
    }

    $kdb = Join-Path $work "java.kdb"
    $report = Join-Path $work "report.txt"
    Invoke-Cli index (Join-Path $Root "testdata\java-inventory") $kdb | Out-Null
    Invoke-Cli analyze $kdb (Join-Path $Root "testdata\java-failure.log") --out $report | Out-Null

    $text = Get-Content $report -Raw -Encoding utf8
    foreach ($expected in @("page.size", "fifty", "inventory.properties")) {
        if ($text -notmatch [regex]::Escape($expected)) { throw "report does not mention '$expected':`n$text" }
    }
    Write-Host "Smoke test passed" -ForegroundColor Green
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
