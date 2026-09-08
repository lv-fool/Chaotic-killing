/*
 * 乱杀跑团 WebView2 客户端
 * - 自动启动同目录 luansha.exe --no-open 8080
 * - 内嵌 WebView2 加载 http://127.0.0.1:8080
 * - 关闭窗口自动结束服务器
 */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>

#include "third_party/webview2/include/WebView2.h"

#define DEFAULT_PORT 8080
#define DEFAULT_URL L"http://127.0.0.1:8080"

static PROCESS_INFORMATION g_serverProc;
static ICoreWebView2Controller *g_controller = NULL;
static ICoreWebView2 *g_webview = NULL;
static HWND g_hwnd = NULL;
static int g_serverReady = 0;

typedef HRESULT (STDMETHODCALLTYPE *CreateCoreWebView2EnvironmentWithOptionsFn)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    IUnknown* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler);

static CreateCoreWebView2EnvironmentWithOptionsFn g_createEnvFn = NULL;

/* ---------- Environment callback ---------- */

typedef struct EnvHandler {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *lpVtbl;
    LONG ref;
} EnvHandler;

static HRESULT STDMETHODCALLTYPE Env_QueryInterface(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, REFIID riid, void **ppv)
{
    EnvHandler *self = (EnvHandler*)This;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
        *ppv = self;
        self->ref++;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Env_AddRef(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This)
{
    EnvHandler *self = (EnvHandler*)This;
    return ++self->ref;
}
static ULONG STDMETHODCALLTYPE Env_Release(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This)
{
    EnvHandler *self = (EnvHandler*)This;
    ULONG r = --self->ref;
    if (r == 0) free(self);
    return r;
}

static HRESULT STDMETHODCALLTYPE Env_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *createdEnvironment);

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl envVtbl = {
    Env_QueryInterface,
    Env_AddRef,
    Env_Release,
    Env_Invoke
};

static EnvHandler *g_envHandler = NULL;

/* ---------- Controller callback ---------- */

typedef struct ControllerHandler {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *lpVtbl;
    LONG ref;
    ICoreWebView2Environment *env;
} ControllerHandler;

static HRESULT STDMETHODCALLTYPE Controller_QueryInterface(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, REFIID riid, void **ppv)
{
    ControllerHandler *self = (ControllerHandler*)This;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
        *ppv = self;
        self->ref++;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Controller_AddRef(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This)
{
    ControllerHandler *self = (ControllerHandler*)This;
    return ++self->ref;
}
static ULONG STDMETHODCALLTYPE Controller_Release(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This)
{
    ControllerHandler *self = (ControllerHandler*)This;
    ULONG r = --self->ref;
    if (r == 0) {
        if (self->env) self->env->lpVtbl->Release(self->env);
        free(self);
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE Controller_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Controller *createdController)
{
    if (errorCode == S_OK && createdController) {
        g_controller = createdController;
        createdController->lpVtbl->get_CoreWebView2(createdController, &g_webview);
        if (g_hwnd) {
            RECT r;
            GetClientRect(g_hwnd, &r);
            createdController->lpVtbl->put_Bounds(createdController, r);
        }
        createdController->lpVtbl->put_IsVisible(createdController, TRUE);
        if (g_webview) {
            g_webview->lpVtbl->Navigate(g_webview, DEFAULT_URL);
        }
    } else {
        wchar_t buf[128];
        swprintf(buf, 128, L"WebView2 控制器创建失败，错误码：0x%08X", (unsigned int)errorCode);
        MessageBoxW(g_hwnd, buf, L"乱杀跑团", MB_OK | MB_ICONERROR);
    }
    return S_OK;
}

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl controllerVtbl = {
    Controller_QueryInterface,
    Controller_AddRef,
    Controller_Release,
    Controller_Invoke
};

static HRESULT STDMETHODCALLTYPE Env_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *createdEnvironment)
{
    if (errorCode == S_OK && createdEnvironment) {
        ControllerHandler *h = calloc(1, sizeof(ControllerHandler));
        if (h) {
            h->lpVtbl = &controllerVtbl;
            h->ref = 1;
            h->env = createdEnvironment;
            createdEnvironment->lpVtbl->AddRef(createdEnvironment);
            createdEnvironment->lpVtbl->CreateCoreWebView2Controller(
                createdEnvironment, g_hwnd,
                (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*)h);
        }
    } else {
        wchar_t buf[128];
        swprintf(buf, 128, L"WebView2 环境创建失败，错误码：0x%08X", (unsigned int)errorCode);
        MessageBoxW(g_hwnd, buf, L"乱杀跑团", MB_OK | MB_ICONERROR);
    }
    return S_OK;
}

/* ---------- server ---------- */

static void start_server(void)
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
        MessageBoxW(g_hwnd, L"启动服务器失败，请确认 luansha.exe 在客户端同目录下。", L"乱杀跑团", MB_OK | MB_ICONERROR);
    } else {
        g_serverReady = 1;
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

/* ---------- window ---------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        start_server();
        return 0;
    case WM_SIZE:
        if (g_controller) {
            RECT r;
            GetClientRect(hwnd, &r);
            if (r.right > r.left && r.bottom > r.top) {
                g_controller->lpVtbl->put_Bounds(g_controller, r);
            }
        }
        return 0;
    case WM_DESTROY:
        if (g_controller) {
            g_controller->lpVtbl->Close(g_controller);
            g_controller->lpVtbl->Release(g_controller);
            g_controller = NULL;
        }
        stop_server();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void init_webview(void)
{
    HMODULE mod = LoadLibraryW(L"WebView2Loader.dll");
    if (!mod) {
        MessageBoxW(g_hwnd, L"找不到 WebView2Loader.dll，请将其放在客户端同目录。", L"乱杀跑团", MB_OK | MB_ICONERROR);
        return;
    }
    g_createEnvFn = (CreateCoreWebView2EnvironmentWithOptionsFn)GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions");
    if (!g_createEnvFn) {
        MessageBoxW(g_hwnd, L"WebView2Loader.dll 缺少入口函数。", L"乱杀跑团", MB_OK | MB_ICONERROR);
        return;
    }
    g_envHandler = calloc(1, sizeof(EnvHandler));
    if (!g_envHandler) return;
    g_envHandler->lpVtbl = &envVtbl;
    g_envHandler->ref = 1;
    g_createEnvFn(NULL, NULL, NULL, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)g_envHandler);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmd, int nShow)
{
    WNDCLASSW wc = {0};
    HWND hwnd;
    MSG msg;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"COM 初始化失败。", L"乱杀跑团", MB_OK | MB_ICONERROR);
        return 1;
    }

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LuanshaWebView2";
    RegisterClassW(&wc);

    hwnd = CreateWindowW(L"LuanshaWebView2", L"乱杀跑团客户端", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                         CW_USEDEFAULT, CW_USEDEFAULT, 1024, 720,
                         NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    /* 等待本地服务器启动完成，避免 WebView 过早导航导致白屏。 */
    Sleep(1200);
    init_webview();

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
