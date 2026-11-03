<#
.SYNOPSIS
    Downloads the third-party sources AppAssist builds against into third_party\ (not committed).

.DESCRIPTION
    - llama.cpp (pinned release): built as static libraries (llama.lib, ggml*.lib) by CMake.
    - nlohmann/json single header (C++11 compatible).
    Downloads resume and retry on a slow or flaky connection. Already-present items are skipped.
#>
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$Root = Split-Path $PSScriptRoot -Parent
$ThirdParty = Join-Path $Root "third_party"

$LlamaTag = "v0.7.0"       # stable release, 2026-10-15; the C API in src\EmbeddedLlamaEngine.cpp matches it
$JsonTag  = "v3.12.0"

function Get-File {
    param([string]$Url, [string]$OutFile)
    & curl.exe -sSL --fail --retry 10 --retry-delay 5 --retry-all-errors --connect-timeout 30 `
        --speed-limit 10240 --speed-time 120 -C - -o $OutFile $Url
    if ($LASTEXITCODE -ne 0) { throw "download failed ($LASTEXITCODE): $Url" }
}

New-Item -ItemType Directory -Force $ThirdParty | Out-Null

$llama = Join-Path $ThirdParty "llama.cpp"
if (-not (Test-Path (Join-Path $llama "include\llama.h"))) {
    Write-Host "Downloading llama.cpp $LlamaTag..."
    $zip = Join-Path $ThirdParty "llama.cpp-$LlamaTag.zip"
    Get-File "https://github.com/ggml-org/llama.cpp/archive/refs/tags/$LlamaTag.zip" $zip
    $tmp = Join-Path $ThirdParty "_llama_extract"
    Remove-Item -Recurse -Force $tmp, $llama -ErrorAction SilentlyContinue
    Expand-Archive $zip -DestinationPath $tmp
    Move-Item (Get-ChildItem $tmp -Directory | Select-Object -First 1).FullName $llama
    Remove-Item -Recurse -Force $tmp, $zip
    Set-Content (Join-Path $llama "PINNED_TAG.txt") $LlamaTag
}

$json = Join-Path $ThirdParty "nlohmann\json.hpp"
if (-not (Test-Path $json)) {
    Write-Host "Downloading nlohmann/json $JsonTag..."
    New-Item -ItemType Directory -Force (Split-Path $json) | Out-Null
    Get-File "https://github.com/nlohmann/json/releases/download/$JsonTag/json.hpp" $json
}

Write-Host "third_party ready: llama.cpp $LlamaTag, nlohmann/json $JsonTag" -ForegroundColor Green
