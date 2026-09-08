/*
 * 乱杀跑团 客户端启动器（稳定版）
 * - 自动启动同目录 luansha.exe --no-open 8080
 * - 点击“打开游戏”用系统浏览器打开
 * - 关闭窗口自动结束服务器
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT 8080
#define DEFAULT_URL L"http://127.0.0.1:8080"

static PROCESS_INFORMATION g_serverProc;

static void start_server(HWND hwnd)
{
    char exe[MAX_PATH];
    char cmd[MAX_PATH];
    STARTUPINFOA si;
    char *slash;

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    slash = strrchr(exe, '\134');
    if (slash) *slash = '\0';
    snprintf(cmd, sizeof(cmd), "%s\134luansha.exe --no-open %d", exe, DEFAULT_PORT);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&g_serverProc, sizeof(g_serverProc));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, exe, &si, &g_serverProc)) {
        MessageBoxW(hwnd, L"启动服务器失败，请确认 luansha.exe 在客户端同目录下。", L"乱杀跑团", MB_OK | MB_ICONERROR);
        return;
    }
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
    ShellExecuteW(hwnd, L"open", DEFAULT_URL, NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        start_server(hwnd);
        CreateWindowW(L"BUTTON", L"打开游戏", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 20, 160, 40, hwnd, (HMENU)1, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"BUTTON", L"退出", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 80, 160, 40, hwnd, (HMENU)2, GetModuleHandle(NULL), NULL);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1: open_game(hwnd); break;
        case 2: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        stop_server();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmd, int nShow)
{
    WNDCLASSW wc = {0};
    HWND hwnd;
    MSG msg;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LuanshaLauncher";
    RegisterClassW(&wc);

    hwnd = CreateWindowW(L"LuanshaLauncher", L"乱杀跑团客户端", WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, 240, 180,
                         NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
