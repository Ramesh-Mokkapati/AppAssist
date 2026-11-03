<#
.SYNOPSIS
    Downloads a recommended GGUF model into models\ (resumable; run it again to continue).

.PARAMETER Model
    Qwen3Coder30B   (default) Qwen3-Coder-30B-A3B-Instruct Q4_K_M, 18.6 GB. Mixture-of-experts: ~3B
                    parameters active per token, so it answers about as fast as a small model.
                    Needs ~24 GB of free RAM. Apache-2.0. GGUF conversion by Unsloth.
    Qwen25Coder7B   Qwen2.5-Coder-7B-Instruct Q4_K_M, 4.7 GB. Needs ~8 GB of free RAM. Apache-2.0.
    Qwen25Coder1_5B Qwen2.5-Coder-1.5B-Instruct Q4_K_M, 1.1 GB. Quick to get; weaker explanations.
#>
param([ValidateSet("Qwen3Coder30B", "Qwen25Coder7B", "Qwen25Coder1_5B")][string]$Model = "Qwen3Coder30B")

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Models = Join-Path $Root "models"
New-Item -ItemType Directory -Force $Models | Out-Null

$catalog = @{
    Qwen3Coder30B   = @{ Url = "https://huggingface.co/unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF/resolve/main/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf"; Size = 18557 }
    Qwen25Coder7B   = @{ Url = "https://huggingface.co/Qwen/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/qwen2.5-coder-7b-instruct-q4_k_m.gguf"; Size = 4683 }
    Qwen25Coder1_5B = @{ Url = "https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF/resolve/main/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf"; Size = 1117 }
}
$item = $catalog[$Model]
$file = Join-Path $Models ($item.Url.Split("/")[-1])
Write-Host "Downloading $($item.Size) MB to $file (resumes if interrupted)..."
& curl.exe -L --fail --retry 50 --retry-delay 10 --retry-all-errors --connect-timeout 30 `
    --speed-limit 10240 --speed-time 120 -C - -o $file $item.Url
if ($LASTEXITCODE -ne 0) { throw "Download stopped (curl exit $LASTEXITCODE). Run this script again to resume." }
Write-Host ("Done: {0} ({1:N0} MB)" -f $file, ((Get-Item $file).Length / 1MB)) -ForegroundColor Green
