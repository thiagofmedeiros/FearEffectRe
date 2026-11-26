# Installs SeedVR2 for FearEffectRe's SeedVR2 video mode into one folder:
#
#   <Dir>\python    portable Python 3.12 (the embeddable package, nothing is
#                   installed on the system and nothing goes in the registry)
#   <Dir>\repo      ComfyUI-SeedVR2_VideoUpscaler (its standalone inference_cli.py)
#   <Dir>\models    the SeedVR2 3B model and its VAE
#
# PyTorch for CUDA 12.6 and the other packages go into that Python. About
# 12 GB is downloaded. Run by the launcher the first time SeedVR2 is chosen:
#   powershell -NoProfile -ExecutionPolicy Bypass -File setup_seedvr2.ps1 -Dir <ai>\seedvr2
# It can be run again: finished steps are skipped.
param([Parameter(Mandatory = $true)][string]$Dir)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Host.UI.RawUI.WindowTitle = 'FearEffectRe - installing SeedVR2'

$PythonVersion = '3.12.8'
$RepoCommit = '4490bd1f482e026674543386bb2a4d176da245b9'
$Models = 'https://huggingface.co/numz/SeedVR2_comfyUI/resolve/main'
$Torch = 'https://download.pytorch.org/whl/cu126'

function Step($text) { Write-Host ''; Write-Host "==> $text" -ForegroundColor Cyan }
function Fetch($url, $file) {
    # curl.exe (part of Windows 10 and 11) resumes and is much faster than Invoke-WebRequest
    & curl.exe -L --fail --retry 5 --retry-delay 5 -C - -o $file $url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
}

try {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $Dir = (Resolve-Path $Dir).Path
    $py = Join-Path $Dir 'python\python.exe'

    Step 'Checking the graphics card'
    if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
        & nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
    } else {
        Write-Host 'No NVIDIA driver found. SeedVR2 needs an NVIDIA graphics card (CUDA); about 24 GB of video memory is recommended.' -ForegroundColor Yellow
    }

    if (-not (Test-Path $py)) {
        Step "Portable Python $PythonVersion"
        $zip = Join-Path $Dir 'python.zip'
        Fetch "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip" $zip
        Expand-Archive -Force $zip (Join-Path $Dir 'python')
        Remove-Item $zip
        # the embeddable package ignores site-packages until "import site" is enabled
        $pth = Get-ChildItem (Join-Path $Dir 'python') -Filter 'python*._pth' | Select-Object -First 1
        # and it ignores PYTHONPATH and the script's own folder: list the SeedVR2 code here
        $lines = (Get-Content $pth.FullName) -replace '^#\s*import site', 'import site'
        $lines + '..\repo' | Set-Content $pth.FullName
        Fetch 'https://bootstrap.pypa.io/get-pip.py' (Join-Path $Dir 'get-pip.py')
        & $py (Join-Path $Dir 'get-pip.py') --no-warn-script-location
        if ($LASTEXITCODE -ne 0) { throw 'pip could not be installed' }
        Remove-Item (Join-Path $Dir 'get-pip.py')
    }

    Step 'PyTorch for CUDA (about 3 GB)'
    & $py -m pip install --no-warn-script-location --default-timeout=300 --retries 10 torch torchvision --index-url $Torch
    if ($LASTEXITCODE -ne 0) { throw 'PyTorch could not be installed' }

    $repo = Join-Path $Dir 'repo'
    if (-not (Test-Path (Join-Path $repo 'inference_cli.py'))) {
        Step 'SeedVR2 code'
        $zip = Join-Path $Dir 'repo.zip'
        Fetch "https://github.com/numz/ComfyUI-SeedVR2_VideoUpscaler/archive/$RepoCommit.zip" $zip
        $tmp = Join-Path $Dir 'repo-unpack'
        Expand-Archive -Force $zip $tmp
        Remove-Item $zip
        if (Test-Path $repo) { Remove-Item -Recurse -Force $repo }
        Move-Item (Get-ChildItem $tmp | Select-Object -First 1).FullName $repo
        Remove-Item -Recurse -Force $tmp
    }

    Step 'SeedVR2 packages'
    & $py -m pip install --no-warn-script-location --default-timeout=300 -r (Join-Path $repo 'requirements.txt')
    if ($LASTEXITCODE -ne 0) { throw 'the SeedVR2 packages could not be installed' }

    $models = Join-Path $Dir 'models\SEEDVR2'
    New-Item -ItemType Directory -Force $models | Out-Null
    foreach ($m in @('ema_vae_fp16.safetensors', 'seedvr2_ema_3b_fp16.safetensors')) {
        $file = Join-Path $models $m
        if (Test-Path $file) { continue }
        Step "Model $m"
        Fetch "$Models/$m" "$file.part"
        Move-Item -Force "$file.part" $file
    }

    Step 'Checking'
    & $py -c "import torch, cv2; print('CUDA available:', torch.cuda.is_available())"
    if ($LASTEXITCODE -ne 0) { throw 'the installed Python does not start' }

    Write-Host ''
    Write-Host 'SeedVR2 is installed. Choose "AI upscale, SeedVR2" and press "Build HD cache..." in the launcher.' -ForegroundColor Green
    Read-Host 'Press Enter to close'
    exit 0
}
catch {
    Write-Host ''
    Write-Host "Installation failed: $_" -ForegroundColor Red
    Write-Host 'Run it again from the launcher to continue where it stopped.'
    Read-Host 'Press Enter to close'
    exit 1
}
