# package.ps1 -- assemble a self-contained, yt-bat-style app folder.
#
# Output layout (dist\ytmerge by default):
#   links.txt     paste YouTube links here
#   RUN.bat       double-click: mp4 + transcript per link into downloaded\
#   README.txt    thirty-second orientation
#   more\         presets: transcripts-only, mp3, audio, update
#   app\          ytmerge.exe + MSYS2 runtime DLLs (the machinery; not user-facing)
#
# The folder is portable: copy it anywhere, it just works. No PATH changes.
#
# Usage (from a normal PowerShell prompt, in the cloned repo):
#   powershell -ExecutionPolicy Bypass -File .\package.ps1
#   powershell -ExecutionPolicy Bypass -File .\package.ps1 -OutDir D:\apps\ytmerge

param(
    [string]$Msys2Root = "",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$RepoDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = Join-Path $RepoDir "dist\ytmerge" }

# --- 1. Locate MSYS2 (same probe as install.ps1) ----------------------------

$candidates = @()
if ($Msys2Root)      { $candidates += $Msys2Root }
if ($env:MSYS2_ROOT) { $candidates += $env:MSYS2_ROOT }
$candidates += @("C:\msys64", "C:\tools\msys64", "$env:LOCALAPPDATA\msys64")

$msys = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path (Join-Path $c "usr\bin\bash.exe"))) { $msys = $c; break }
}
if (-not $msys) {
    Write-Host "Error: MSYS2 not found. Run install.ps1 first (it walks you through setup)." -ForegroundColor Red
    exit 1
}
$bash = Join-Path $msys "usr\bin\bash.exe"

function Invoke-Ucrt64([string]$cmd) {
    $env:MSYSTEM = "UCRT64"; $env:CHERE_INVOKING = "1"
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

# --- 2. Build ---------------------------------------------------------------

Write-Host "Building ytmerge..."
Invoke-Ucrt64 "make"
$exe = Join-Path $RepoDir "ytmerge.exe"
if (-not (Test-Path $exe)) {
    Write-Host "Error: build did not produce ytmerge.exe." -ForegroundColor Red
    exit 1
}

# --- 3. Assemble app\ (exe + DLLs, ldd-enumerated like install.ps1) ---------

$appDir = Join-Path $OutDir "app"
New-Item -ItemType Directory -Force $appDir | Out-Null
Copy-Item $exe (Join-Path $appDir "ytmerge.exe") -Force

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
        Copy-Item $dll $appDir -Force
        $copied++
    }
}
Write-Host "app\: ytmerge.exe + $copied DLLs"

# --- 4. User-facing files ---------------------------------------------------

$moreDir = Join-Path $OutDir "more"
New-Item -ItemType Directory -Force $moreDir | Out-Null

# links.txt: only write the template if the user doesn't already have one,
# so re-packaging over an in-use folder never clobbers their list.
$links = Join-Path $OutDir "links.txt"
if (-not (Test-Path $links)) {
    Set-Content -Encoding utf8 $links @"
# 1. Paste YouTube links below, one per line (messy text is fine).
# 2. Double-click RUN.bat.
# 3. Videos + transcripts appear in the downloaded folder.
"@
}

Set-Content -Encoding ascii (Join-Path $OutDir "RUN.bat") @"
@echo off
cd /D "%~dp0"
app\ytmerge.exe links.txt
echo.
echo Finished (you can now close this window)
pause >nul
"@

function Write-Preset([string]$name, [string]$flags, [string]$blurb) {
    Set-Content -Encoding ascii (Join-Path $moreDir $name) @"
@echo off
rem $blurb
cd /D "%~dp0.."
app\ytmerge.exe $flags links.txt
echo.
echo Finished (you can now close this window)
pause >nul
"@
}
Write-Preset "transcripts-only.bat" "--no-video" "Transcripts + metadata only -- fast, no video downloads."
Write-Preset "mp3.bat"              "--mp3"      "Audio only, converted to mp3."
Write-Preset "audio.bat"            "--audio"    "Audio only, best available format."

Set-Content -Encoding ascii (Join-Path $moreDir "update.bat") @"
@echo off
rem Force a yt-dlp self-update check right now (normally automatic, daily).
cd /D "%~dp0.."
app\ytmerge.exe --update
echo.
echo Finished (you can now close this window)
pause >nul
"@

Set-Content -Encoding utf8 (Join-Path $OutDir "README.txt") @"
ytmerge -- YouTube videos + AI-ready transcripts, from a list of links
======================================================================

HOW TO USE
  1. Open links.txt and paste your YouTube links (one per line; messy text is fine).
  2. Double-click RUN.bat.
  3. Everything lands in the downloaded folder: for each video an .mp4 AND a .txt
     with the full transcript ([m:ss] timestamps + title/channel/date/chapters/
     description). The merged transcripts are also placed on your clipboard,
     ready to paste into an AI.

WHAT'S IN THIS FOLDER
  links.txt    your list of links
  RUN.bat      the main button
  downloaded\  results appear here
  more\        presets: transcripts-only (fast, no video), mp3, audio, update
  app\         the program itself -- you never need to open this

TIPS
  - Re-running is cheap: already-downloaded videos are skipped and transcripts
    come from a 7-day cache.
  - You can also run app\ytmerge.exe from a terminal with just URLs copied to
    your clipboard -- no links.txt needed. Run it with -h for all options.
  - If a big batch starts failing with "ip blocked (429)", YouTube has
    rate-limited your IP for transcripts: wait a bit, switch networks, or see
    the proxy options in -h.

Source, updates, and full docs: https://github.com/mal0ware/ytmerge
"@

Write-Host ""
Write-Host "Packaged: $OutDir"
Get-ChildItem $OutDir | ForEach-Object { Write-Host "  $($_.Name)" }
