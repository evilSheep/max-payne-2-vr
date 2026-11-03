@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0bindings\Fix-Frame-Controls.ps1" > "%~dp0Frame Controls Fix.log" 2>&1
set "mp2vrFrameExit=%errorlevel%"
type "%~dp0Frame Controls Fix.log"
if not "%mp2vrFrameExit%"=="0" (
  echo.
    echo The fix could not complete. Please send Frame Controls Fix.log.
)
pause
exit /b %mp2vrFrameExit%
