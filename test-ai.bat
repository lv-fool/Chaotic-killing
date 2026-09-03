@echo off
setlocal

REM 测试脚本：需要先配置 LUANSHA_AI_KEY 环境变量（或本机已设置）。
if defined LUANSHA_AI_KEY (
    set "KEY=%LUANSHA_AI_KEY%"
) else (
    set "KEY=YOUR_KEY_HERE"
    echo [WARN] 未检测到 LUANSHA_AI_KEY，使用占位符测试会失败。
    echo 请先执行： set LUANSHA_AI_KEY=sk-xxxx
)

set "MODEL=sensenova-6.8-flash-lite"

echo === Test 1: api.sensenova.cn/v1/chat/completions ===
curl -v -m 30 -X POST "https://api.sensenova.cn/v1/chat/completions" -H "Content-Type: application/json" -H "Authorization: Bearer %KEY%" -d "{\"model\":\"%MODEL%\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}"
echo.

echo === Test 2: api.sensenova.cn/v1/llm/chat-completions ===
curl -v -m 30 -X POST "https://api.sensenova.cn/v1/llm/chat-completions" -H "Content-Type: application/json" -H "Authorization: Bearer %KEY%" -d "{\"model\":\"%MODEL%\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}"
echo.

echo === Test 3: token.sensenova.cn/v1/llm/chat-completions ===
curl -v -m 30 -X POST "https://token.sensenova.cn/v1/llm/chat-completions" -H "Content-Type: application/json" -H "Authorization: Bearer %KEY%" -d "{\"model\":\"%MODEL%\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}"
echo.

echo === Test 4: token.sensenova.cn/v1/chat/completions ===
curl -v -m 30 -X POST "https://token.sensenova.cn/v1/chat/completions" -H "Content-Type: application/json" -H "Authorization: Bearer %KEY%" -d "{\"model\":\"%MODEL%\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}"
echo.

echo Done.
pause
endlocal
