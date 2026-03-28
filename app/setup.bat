@echo off
set "EXE=%~dp0ClipCrush.exe"

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" ^
    /v "ClipCrush" /t REG_SZ /d "\"%EXE%\"" /f >nul

echo.
echo  [OK] ClipCrush will now start automatically with Windows.
echo       It will run from: %EXE%
echo.
echo  Note: If you move the ClipCrush folder, run this bat again.
echo.
echo  Starting ClipCrush...
start "" "%EXE%"
timeout /t 2 >nul