@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0bindings\Fix-Frame-Controls.ps1" -Restore > "%~dp0Frame Controls Restore.log" 2>&1
set "mp2vrFrameExit=%errorlevel%"
type "%~dp0Frame Controls Restore.log"
if not "%mp2vrFrameExit%"=="0" echo Could not complete. Please send Frame Controls Restore.log.
pause
exit /b %mp2vrFrameExit%
