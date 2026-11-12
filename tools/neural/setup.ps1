# One-time setup for tools/neural (Windows). Re-running it is safe.
#   powershell -ExecutionPolicy Bypass -File tools\neural\setup.ps1
# Creates tools\neural\.venv (gitignored) on Python 3.10-3.12, installs CUDA torch from the PyTorch index,
# then the rest of requirements.txt from PyPI.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$torchIndex = "https://download.pytorch.org/whl/cu126"

# 1. A Python 3.10-3.12: the py launcher first, then common install paths.
$py = $null
foreach ($v in "3.12", "3.11", "3.10") {
    try { & py "-$v" -c "import sys" 2>$null; if ($LASTEXITCODE -eq 0) { $py = @("py", "-$v"); break } } catch {}
}
if (-not $py) {
    $cand = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python31[012]\python.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cand) { $py = @($cand.FullName) }
}
if (-not $py) { throw "Python 3.10-3.12 not found: install one (winget install Python.Python.3.12)" }

$venv = Join-Path $here ".venv"
if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
    Write-Host "creating $venv with $($py -join ' ')"
    $pyArgs = @()
    if ($py.Length -gt 1) { $pyArgs = $py[1..($py.Length - 1)] }
    & $py[0] @pyArgs -m venv $venv
}
$vpy = Join-Path $venv "Scripts\python.exe"
& $vpy -m pip install --upgrade pip -q

# 2. CUDA torch from the PyTorch index (PyPI wheels are CPU-only on Windows), then the rest from PyPI.
& $vpy -m pip install torch torchvision --index-url $torchIndex
& $vpy -m pip install -r (Join-Path $here "requirements.txt")

& $vpy -c "import torch; print('torch', torch.__version__, 'cuda', torch.cuda.is_available())"
Write-Host "done. Use: tools\neural\.venv\Scripts\python"
