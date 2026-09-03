#include "game.h"
#include "server.h"
#include "ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#endif

static void set_default_env(const char *name, const char *value)
{
    const char *cur = getenv(name);
    if (!cur || !*cur) {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 0);
#endif
    }
}

int main(int argc, char **argv)
{
    int port = 8080;
    int open_browser = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-open") == 0) {
            open_browser = 0;
        } else {
            port = atoi(argv[i]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "端口无效: %s\n", argv[i]);
                return 1;
            }
        }
    }

    /* AI 接口默认仅设置 URL/模型；Key 必须通过环境变量或页面弹窗提供，
       绝不写死在代码里（Phase 0 安全清理）。未配置 Key 时自动回退关键词兜底（见 ai.c）。 */
    set_default_env("LUANSHA_AI_URL", "https://token.sensenova.cn/v1/chat/completions");
    set_default_env("LUANSHA_AI_MODEL", "sensenova-6.8-flash-lite");

    game_init();
    ai_init();   /* 读取 ./luansha_ai.conf（若存在），运行时/文件配置优先于环境变量 */

    printf("[AI] URL=%s\n", ai_get_url() ? ai_get_url() : "(default)");
    printf("[AI] MODEL=%s\n", ai_get_model() ? ai_get_model() : "(default)");
    {
        const char *k = ai_get_key();
        printf("[AI] KEY=%s\n", (k && *k) ? "(set)" : "(not set - 使用关键词兜底)");
    }
    printf("[AI] DEBUG=%s\n", getenv("LUANSHA_AI_DEBUG") ? getenv("LUANSHA_AI_DEBUG") : "(off)");
    fflush(stdout);

    return server_run_ex(port, open_browser);
}