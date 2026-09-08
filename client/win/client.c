/*
 * 乱杀跑团 客户端启动器（Step 1 中间形态）
 *
 * 功能：
 * - 自动启动同目录下的 luansha.exe --no-open 8080
 * - 点击“打开游戏”打开系统默认浏览器
 * - 关闭窗口时自动结束服务器进程
 *
 * 说明：
 * - 当前先使用系统浏览器，后续接入 WebView2 后改为内嵌窗口。
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT 8080
#define DEFAULT_URL "http://127.0.0.1:8080"

static PROCESS_INFORMATION g_serverProc;

static void start_server(HWND hwnd)
{
    char exe[MAX_PATH];
    char cmd[MAX_PATH];
    STARTUPINFO si;

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    /* 去掉文件名，得到目录 */
    {
        char *slash = strrchr(exe, '\');
        if (slash) *slash = '\0';
    }
    snprintf(cmd, sizeof(cmd), "%s\luansha.exe --no-open %d", exe, DEFAULT_PORT);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&g_serverProc, sizeof(g_serverProc));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, exe, &si, &g_serverProc)) {
        MessageBoxA(hwnd, "启动服务器失败，请确认 luansha.exe 在客户端同目录下。", "乱杀跑团", MB_OK | MB_ICONERROR);
        return;
    }

    MessageBoxA(hwnd, "服务器已启动。\n请点击“打开游戏”。", "乱杀跑团", MB_OK | MB_ICONINFORMATION);
}

static void stop_server(void)
{
    if (g_serverProc.hProcess) {
        TerminateProcess(g_serverProc.hProcess, 0);
        CloseHandle(g_serverProc.hProcess);
        CloseHandle(g_serverProc.hThread);
        ZeroMemory(&g_serverProc, sizeof(g_serverProc));
    }
}

static void open_game(HWND hwnd)
{
    ShellExecuteA(hwnd, "open", DEFAULT_URL, NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateWindowA("BUTTON", "启动服务器", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 20, 140, 36, hwnd, (HMENU)1, GetModuleHandle(NULL), NULL);
        CreateWindowA("BUTTON", "打开游戏", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 70, 140, 36, hwnd, (HMENU)2, GetModuleHandle(NULL), NULL);
        CreateWindowA("BUTTON", "退出", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 120, 140, 36, hwnd, (HMENU)3, GetModuleHandle(NULL), NULL);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1: start_server(hwnd); break;
        case 2: open_game(hwnd); break;
        case 3: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        stop_server();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSA wc = {0};
    HWND hwnd;
    MSG msg;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "LuanshaClient";
    RegisterClassA(&wc);

    hwnd = CreateWindowA("LuanshaClient", "乱杀跑团客户端", WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, 220, 220,
                         NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nShow);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
}
