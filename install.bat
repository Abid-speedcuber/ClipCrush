@echo off
setlocal enabledelayedexpansion
title ClipCrush Installer

:: ── Check admin ──────────────────────────────────────────────────────────────
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] Please run as Administrator.
    pause & exit /b 1
)

:: ── Paths ─────────────────────────────────────────────────────────────────────
set "INSTALL_DIR=%ProgramFiles%\ClipCrush"
set "SRC_DIR=%~dp0"

echo.
echo  ==========================================
echo    ClipCrush Installer
echo  ==========================================
echo.
echo  Install dir: %INSTALL_DIR%
echo.

mkdir "%INSTALL_DIR%" 2>nul

:: ── Download FFmpeg (static build, ~70 MB) ────────────────────────────────────
echo  [1/4] Checking for FFmpeg...
if exist "%INSTALL_DIR%\ffmpeg.exe" (
    echo        ffmpeg.exe already present, skipping download.
) else (
    echo  [1/4] Downloading FFmpeg static build...
    echo        This may take a minute depending on your connection.
    echo.

    :: Use PowerShell to download ffmpeg from GitHub releases (gyan.dev static build)
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$url='https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip';" ^
        "$out='%TEMP%\ffmpeg_build.zip';" ^
        "Write-Host '  Downloading...' -NoNewline;" ^
        "Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing;" ^
        "Write-Host ' done.'"

    if %errorlevel% neq 0 (
        echo.
        echo  [!] Download failed. Check your internet connection.
        echo      Alternatively, manually place ffmpeg.exe in:
        echo      %INSTALL_DIR%
        pause & exit /b 1
    )

    echo  [1/4] Extracting ffmpeg.exe...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "Add-Type -Assembly System.IO.Compression.FileSystem;" ^
        "$zip=[IO.Compression.ZipFile]::OpenRead('%TEMP%\ffmpeg_build.zip');" ^
        "foreach($e in $zip.Entries){ if($e.Name -eq 'ffmpeg.exe'){ [IO.Compression.ZipFileExtensions]::ExtractToFile($e,'%INSTALL_DIR%\ffmpeg.exe',$true); break } };" ^
        "$zip.Dispose();" ^
        "Write-Host '  ffmpeg.exe extracted.'"

    del /q "%TEMP%\ffmpeg_build.zip" 2>nul
)

:: ── Compile ClipCrush.cpp ─────────────────────────────────────────────────────
echo.
echo  [2/4] Compiling ClipCrush.cpp...

:: Try g++ (MinGW) first
where g++ >nul 2>&1
if %errorlevel% equ 0 (
    g++ -O2 -std=c++17 -o "%INSTALL_DIR%\ClipCrush.exe" ^
        "%SRC_DIR%ClipCrush.cpp" ^
        -lole32 -lshell32 -luser32 -lwinmm -mwindows
    if %errorlevel% neq 0 (
        echo.
        echo  [!] Compilation failed. See errors above.
        echo      Fix the errors and re-run install.bat.
        pause
        exit /b 1
    )
    echo       Compiled with g++.
    goto compiled
)

:: Try cl.exe (MSVC) 
where cl >nul 2>&1
if %errorlevel% equ 0 (
    cl /O2 /EHsc /Fe:"%INSTALL_DIR%\ClipCrush.exe" ^
        "%SRC_DIR%ClipCrush.cpp" ^
        /link ole32.lib shell32.lib user32.lib /SUBSYSTEM:WINDOWS
    if %errorlevel% neq 0 (
        echo  [!] Compilation failed. See errors above.
        pause & exit /b 1
    )
    echo       Compiled with MSVC.
    goto compiled
)

:: No compiler found — download MinGW via winget
echo       No C++ compiler found. Attempting to install MinGW via winget...
winget install -e --id MSYS2.MSYS2 --silent
if %errorlevel% neq 0 (
    echo.
    echo  [!] Could not auto-install compiler. Please install one of:
    echo      - MinGW-w64: https://winlibs.com/
    echo      - MSVC: https://visualstudio.microsoft.com/downloads/
    echo      Then re-run install.bat.
    pause & exit /b 1
)
:: After MSYS2, user needs to open MSYS2 shell — guide them
echo.
echo  MinGW installed. Please:
echo   1. Open MSYS2 UCRT64 from Start Menu
echo   2. Run:  pacman -S mingw-w64-ucrt-x86_64-gcc
echo   3. Add C:\msys64\ucrt64\bin to your system PATH
echo   4. Re-run install.bat
pause & exit /b 0

:compiled

:: ── Create startup shortcut (runs silently in background) ────────────────────
echo.
echo  [3/4] Installing startup entry...

set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "SHORTCUT=%STARTUP%\ClipCrush.lnk"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ws=New-Object -ComObject WScript.Shell;" ^
    "$sc=$ws.CreateShortcut('%SHORTCUT%');" ^
    "$sc.TargetPath='%INSTALL_DIR%\ClipCrush.exe';" ^
    "$sc.WorkingDirectory='%INSTALL_DIR%';" ^
    "$sc.WindowStyle=7;" ^
    "$sc.Description='ClipCrush - Clipboard video compressor';" ^
    "$sc.Save();" ^
    "Write-Host '  Startup shortcut created.'"

:: ── Launch now ────────────────────────────────────────────────────────────────
echo.
echo  [4/4] Launching ClipCrush...
start "" /min "%INSTALL_DIR%\ClipCrush.exe"

echo.
echo  ==========================================
echo   Installation complete!
echo  ==========================================
echo.
echo   ClipCrush is now running in the background.
echo   It will auto-start with Windows.
echo.
echo   Usage:
echo     1. Copy a video file  (right-click - Copy)
echo     2. Click inside Discord / wherever you want to send it
echo     3. Press  Ctrl + Alt + Shift + V
echo     4. Watch the progress bar -- file pastes automatically!
echo.
echo   Output files are saved next to the original
echo   with suffix _compressed.mp4
echo.
pause