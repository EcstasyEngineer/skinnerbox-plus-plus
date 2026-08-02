# Install the GPT-2 lab host for SkinnerBox++ Advanced debug (LAB).
#
# The plugin does not ship model weights or a Python runtime. This script:
#   1. Copies the lab host scripts into the Notepad++ config tree
#   2. Points SkinnerBoxPP.ini [lab] at those scripts + the experiments venv
#   3. Optionally copies build\SkinnerBoxPP.dll into the NPP plugins folder
#   4. Verifies the host answers {"present":...} for GPT-2
#
# Usage (from repo root):
#   powershell -ExecutionPolicy Bypass -File tools\setup_lab.ps1
#   powershell -ExecutionPolicy Bypass -File tools\setup_lab.ps1 -InstallDll
param(
    [switch]$InstallDll,
    [string]$Python = "",
    [string]$NppPlugins = ""
)

$ErrorActionPreference = "Stop"
# tools\setup_lab.ps1 → repo root
$Repo = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $Repo "experiments\gpt2_lab_host.py"))) {
    throw "run from repo checkout (expected experiments\gpt2_lab_host.py under $Repo)"
}
Set-Location $Repo

$LabSrc = Join-Path $Repo "experiments"
$VenvPy = Join-Path $LabSrc ".venv\Scripts\python.exe"
$ConfigDir = Join-Path $env:APPDATA "Notepad++\plugins\config"
$LabDir = Join-Path $ConfigDir "SkinnerBoxPP-lab"
$IniPath = Join-Path $ConfigDir "SkinnerBoxPP.ini"

Write-Host "repo:     $Repo"
Write-Host "lab dir:  $LabDir"
Write-Host "ini:      $IniPath"

if (-not (Test-Path $VenvPy)) {
    Write-Host "creating experiments\.venv ..."
    python -m venv (Join-Path $LabSrc ".venv")
    if (-not (Test-Path $VenvPy)) { throw "venv python missing after create" }
}

if ($Python -eq "") { $Python = $VenvPy }
if (-not (Test-Path $Python)) { throw "python not found: $Python" }

Write-Host "ensuring torch / transformers / nltk / numpy ..."
& $Python -m pip install --upgrade pip -q
& $Python -m pip install "torch" "transformers" "nltk" "numpy" -q
& $Python -c "import nltk; nltk.download('gutenberg', quiet=True); nltk.download('brown', quiet=True); print('nltk ok')"

New-Item -ItemType Directory -Force -Path $LabDir | Out-Null
$files = @("gpt2_lab_host.py", "gpt2_lab.py", "corpus.py")
foreach ($f in $files) {
    $src = Join-Path $LabSrc $f
    if (-not (Test-Path $src)) { throw "missing $src" }
    Copy-Item $src (Join-Path $LabDir $f) -Force
    Write-Host "  copied $f"
}

# Pin absolute paths in the INI so Program Files install still finds the lab.
$hostPy = Join-Path $LabDir "gpt2_lab_host.py"
if (-not (Test-Path $ConfigDir)) {
    New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
}
if (-not (Test-Path $IniPath)) {
    @"
[general]
enabled=1
"@ | Set-Content -Path $IniPath -Encoding ASCII
}

function Set-IniValue([string]$path, [string]$section, [string]$key, [string]$value) {
    $content = @()
    if (Test-Path $path) { $content = Get-Content $path }
    $out = New-Object System.Collections.Generic.List[string]
    $inSection = $false
    $sectionFound = $false
    $keyWritten = $false
    foreach ($line in $content) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            if ($inSection -and -not $keyWritten) {
                $out.Add("$key=$value")
                $keyWritten = $true
            }
            $inSection = ($Matches[1] -eq $section)
            if ($inSection) { $sectionFound = $true }
            $out.Add($line)
            continue
        }
        if ($inSection -and $line -match "^\s*$key\s*=") {
            $out.Add("$key=$value")
            $keyWritten = $true
            continue
        }
        $out.Add($line)
    }
    if ($inSection -and -not $keyWritten) {
        $out.Add("$key=$value")
        $keyWritten = $true
    }
    if (-not $sectionFound) {
        $out.Add("[$section]")
        $out.Add("$key=$value")
    }
    $out | Set-Content -Path $path -Encoding ASCII
}

Set-IniValue $IniPath "lab" "python" $Python
Set-IniValue $IniPath "lab" "host" $hostPy
Set-IniValue $IniPath "telemetry" "advanced_debug" "0"
Write-Host "INI [lab] python=$Python"
Write-Host "INI [lab] host=$hostPy"

Write-Host "verifying host --check ..."
# HF/torch write progress to stderr; don't treat that as failure.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$checkLines = & $Python -u $hostPy --check 2>&1 | ForEach-Object { "$_" }
$ErrorActionPreference = $prevEap
$check = ($checkLines | Where-Object { $_ -match '"present"' } | Select-Object -Last 1)
Write-Host ($checkLines -join "`n")
if (-not $check) {
    throw "host check failed (no present field). output: $($checkLines -join ' | ')"
}
if ($check -match '"present"\s*:\s*true') {
    Write-Host "GPT-2 already in local HF cache."
} else {
    Write-Host "GPT-2 not cached yet - plugin will prompt on first LAB arm."
}

if ($InstallDll) {
    if ($NppPlugins -eq "") {
        $NppPlugins = Join-Path ${env:ProgramFiles} "Notepad++\plugins"
    }
    $destDir = Join-Path $NppPlugins "SkinnerBoxPP"
    $dllSrc = Join-Path $Repo "build\SkinnerBoxPP.dll"
    if (-not (Test-Path $dllSrc)) { throw "build\SkinnerBoxPP.dll missing - run build.bat first" }
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    $destDll = Join-Path $destDir "SkinnerBoxPP.dll"
    try {
        Copy-Item $dllSrc $destDll -Force
        Write-Host "installed DLL -> $destDll"
    } catch {
        Write-Host "WARNING: could not copy DLL (close Notepad++ / need admin?): $_"
        Write-Host "  copy manually: $dllSrc -> $destDir\"
    }
}

Write-Host ""
Write-Host "Lab host is set up."
Write-Host "  1. Restart Notepad++ (if it was open)"
Write-Host "  2. Plugins -> SkinnerBox++ -> Advanced debug LAB"
Write-Host "  3. First arm prompts to download GPT-2 if not cached (~500 MB)"
Write-Host "Done."
