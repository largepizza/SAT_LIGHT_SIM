# One-time setup for tools/video (Windows). Re-running it is safe.
#   powershell -ExecutionPolicy Bypass -File tools\video\setup.ps1
# Creates tools\video\.venv (gitignored) on a Python Manim supports, installs requirements.txt, and makes sure
# LaTeX (MiKTeX, for MathTex) and ffmpeg are present.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# 1. A Python Manim has wheels for (3.10-3.12): the py launcher first, then common install paths.
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
& $vpy -m pip install -r (Join-Path $here "requirements.txt")

# 2. LaTeX for Manim's MathTex. MiKTeX installs per user; packages are fetched on first use.
$tex = Join-Path $env:LOCALAPPDATA "Programs\MiKTeX\miktex\bin\x64"
if (-not (Get-Command latex -ErrorAction SilentlyContinue) -and -not (Test-Path (Join-Path $tex "latex.exe"))) {
    Write-Host "installing MiKTeX"
    winget install --id MiKTeX.MiKTeX -e --accept-source-agreements --accept-package-agreements --silent
}
if (Test-Path (Join-Path $tex "initexmf.exe")) {
    & (Join-Path $tex "initexmf.exe") --set-config-value "[MPM]AutoInstall=1" | Out-Null
    & (Join-Path $tex "miktex.exe") packages check-update | Out-Null
}

# 3. ffmpeg (the edit) — expected on PATH.
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Host "installing ffmpeg"
    winget install --id Gyan.FFmpeg -e --accept-source-agreements --accept-package-agreements --silent
}

& $vpy -c "import manim, vidkit; print('manim', manim.__version__, '- ok')" 2>$null
if ($LASTEXITCODE -ne 0) { Push-Location $here; & $vpy -c "import manim, vidkit; print('manim', manim.__version__, '- ok')"; Pop-Location }
Write-Host "done. From tools\video:  .venv\Scripts\python -m vidkit list"
