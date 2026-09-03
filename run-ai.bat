@echo off
setlocal

echo [1/2] Building...
call "%~dp0build.bat"
if errorlevel 1 (
    echo Build failed.
    pause
    exit /b 1
)

echo [2/2] Starting with Sensenova AI...
set "LUANSHA_AI_URL=https://token.sensenova.cn/v1/chat/completions"
set "LUANSHA_AI_MODEL=sensenova-6.8-flash-lite"

REM Key 从用户目录文件读取（%USERPROFILE%\.luansha_key），不写死在脚本/源码里。
REM 未配置 Key 时，程序自动回退关键词兜底，不会卡住。
if exist "%USERPROFILE%\.luansha_key" (
    set /p LUANSHA_AI_KEY=<"%USERPROFILE%\.luansha_key"
)

"%~dp0luansha.exe" %*
if errorlevel 1 pause

endlocal
