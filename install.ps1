# install.ps1 -- Windows installer for ytmerge (mirrors install.sh).
#
# What it does:
#   1. Locates an existing MSYS2 installation (like install.sh requires Homebrew on macOS).
#   2. Installs the build deps via pacman: UCRT64 toolchain, curl, nlohmann-json, make, pkgconf.
#   3. Builds with `make` in the UCRT64 environment.
#   4. Installs ytmerge.exe plus its required MSYS2 runtime DLLs to
#      %LOCALAPPDATA%\ytmerge\bin and adds that directory to the user PATH.
#
# Usage (from a normal PowerShell prompt, in the cloned repo):
#   powershell -ExecutionPolicy Bypass -File .\install.ps1
#   # or, if MSYS2 lives somewhere unusual:
#   powershell -ExecutionPolicy Bypass -File .\install.ps1 -Msys2Root D:\msys64

param(
    [string]$Msys2Root = ""
)

$ErrorActionPreference = "Stop"

$RepoDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# --- 1. Locate MSYS2 --------------------------------------------------------

$candidates = @()
if ($Msys2Root)      { $candidates += $Msys2Root }
if ($env:MSYS2_ROOT) { $candidates += $env:MSYS2_ROOT }
$candidates += @("C:\msys64", "C:\tools\msys64", "$env:LOCALAPPDATA\msys64")

$msys = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path (Join-Path $c "usr\bin\bash.exe"))) { $msys = $c; break }
}

if (-not $msys) {
    Write-Host "Error: MSYS2 not found (looked in: $($candidates -join ', '))." -ForegroundColor Red
    Write-Host "Install it first, then re-run this script:"
    Write-Host "    winget install MSYS2.MSYS2"
    Write-Host "or download from https://www.msys2.org/"
    Write-Host "If MSYS2 is installed somewhere else, pass -Msys2Root <path>."
    exit 1
}
Write-Host "Using MSYS2 at $msys"

$bash = Join-Path $msys "usr\bin\bash.exe"

# Run a command inside the MSYS2 UCRT64 environment, starting in the repo dir.
function Invoke-Ucrt64([string]$cmd) {
    $env:MSYSTEM       = "UCRT64"
    $env:CHERE_INVOKING = "1"   # keep bash's cwd = PowerShell's cwd
    Push-Location $RepoDir
    try {
        & $bash -lc $cmd
        if ($LASTEXITCODE -ne 0) { throw "MSYS2 command failed (exit $LASTEXITCODE): $cmd" }
    } finally {
        Pop-Location
        Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
        Remove-Item Env:CHERE_INVOKING -ErrorAction SilentlyContinue
    }
}

# --- 2. Install build dependencies via pacman -------------------------------

Write-Host "Installing build dependencies via pacman..."
Invoke-Ucrt64 "pacman -S --needed --noconfirm make pkgconf mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-nlohmann-json"

# --- 3. Build ---------------------------------------------------------------

Write-Host "Building ytmerge..."
Invoke-Ucrt64 "make clean >/dev/null 2>&1 || true; make"

$exe = Join-Path $RepoDir "ytmerge.exe"
if (-not (Test-Path $exe)) {
    Write-Host "Error: build did not produce ytmerge.exe." -ForegroundColor Red
    exit 1
}

# --- 4. Install exe + runtime DLLs to a PATH dir ----------------------------

$installDir = Join-Path $env:LOCALAPPDATA "ytmerge\bin"
New-Item -ItemType Directory -Force $installDir | Out-Null

Copy-Item $exe (Join-Path $installDir "ytmerge.exe") -Force

# The MinGW-built exe depends on MSYS2 UCRT64 DLLs (libcurl-4, libstdc++-6, ...).
# Enumerate them with ldd and copy them next to the exe so it runs from any shell.
Write-Host "Copying required MSYS2 runtime DLLs..."
$env:MSYSTEM = "UCRT64"; $env:CHERE_INVOKING = "1"
Push-Location $RepoDir
try {
    $lddCmd = 'ldd ytmerge.exe | awk ''/=> \/ucrt64\//{print $3}'' | while read -r f; do cygpath -w "$f"; done'
    $dllPaths = & $bash -lc $lddCmd
    if ($LASTEXITCODE -ne 0) { throw "ldd failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
    Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
    Remove-Item Env:CHERE_INVOKING -ErrorAction SilentlyContinue
}
$copied = 0
foreach ($dll in @($dllPaths)) {
    $dll = "$dll".Trim()
    if ($dll -and (Test-Path $dll)) {
        Copy-Item $dll $installDir -Force
        $copied++
    }
}
Write-Host "Copied ytmerge.exe + $copied DLLs to $installDir"

# --- 5. Ensure install dir is on the user PATH ------------------------------

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (($userPath -split ";") -notcontains $installDir) {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$installDir", "User")
    Write-Host "Added $installDir to your user PATH (takes effect in NEW terminals)."
} else {
    Write-Host "$installDir is already on your user PATH."
}

# --- 6. Smoke test ----------------------------------------------------------

Write-Host ""
& (Join-Path $installDir "ytmerge.exe") --help
if ($LASTEXITCODE -ne 0) {
    Write-Host "Warning: 'ytmerge --help' exited with code $LASTEXITCODE." -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "Done. Open a new terminal and test with: copy a YouTube URL, then run ``ytmerge``"
Write-Host "Next: bind it to a hotkey (PowerToys Keyboard Manager or AutoHotkey -- see README)."
