@echo off
setlocal

echo Stopping old server if running...
taskkill /F /IM luansha.exe >nul 2>&1
timeout /t 1 /nobreak >nul

set "LUANSHA_AI_DEBUG=1"

echo Starting with AI debug output...
"%~dp0luansha.exe" %*
if errorlevel 1 pause

endlocal