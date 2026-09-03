@echo off
setlocal
set "PATH=D:\msys64\mingw64\bin;D:\msys64\usr\bin;%PATH%"

gcc -std=c99 -Wall -Wextra -O2 -o luansha.exe src\main.c src\server.c src\game.c src\json.c src\ai.c src\web_assets.c -lws2_32 -lshell32
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Build OK: luansha.exe
endlocal