import os
import subprocess
import sys
import threading
import time
import urllib.request

PORT = 8080
URL = f"http://127.0.0.1:{PORT}"

# 本机探活必须直连，绕开系统/环境代理。
# 否则 http_proxy 一旦存在，127.0.0.1 的请求会被代理拦截成 5xx，
# 服务器明明起来了却一直判定为「启动超时」。
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def base_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def start_server():
    exe = None
    for d in (base_dir(), os.getcwd()):
        cand = os.path.join(d, "luansha.exe")
        if os.path.exists(cand):
            exe = cand
            break
    if not exe:
        raise RuntimeError("找不到 luansha.exe，请将它放在客户端同目录。")
    proc = subprocess.Popen(
        [exe, "--no-open", str(PORT)],
        cwd=os.path.dirname(exe),
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    return proc


def wait_server(timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            _OPENER.open(URL, timeout=1)
            return True
        except Exception:
            time.sleep(0.3)
    return False


class Api:
    def __init__(self):
        self.tunnel_proc = None
        self.tunnel_url = ''

    def quit_app(self):
        self.stop_tunnel()
        import webview
        for w in webview.windows:
            try:
                w.destroy()
            except Exception:
                pass

    def start_tunnel(self):
        exe = None
        for d in (base_dir(), os.getcwd()):
            cand = os.path.join(d, "cloudflared.exe")
            if os.path.exists(cand):
                exe = cand
                break
        if not exe:
            return {"ok": False, "error": "缺少 cloudflared.exe，请将它放在客户端同目录。"}

        if self.tunnel_proc and self.tunnel_proc.poll() is None:
            return {"ok": True, "url": self.tunnel_url}

        self.tunnel_url = ''
        self.tunnel_log_path = os.path.join(os.path.dirname(exe), "cloudflared.log")
        proc = subprocess.Popen(
            [exe, "tunnel", "--url", URL, "--no-autoupdate", "--protocol", "http2", "--edge-ip-version", "4"],
            cwd=os.path.dirname(exe),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0,
        )
        self.tunnel_proc = proc

        def reader(p):
            try:
                with open(self.tunnel_log_path, "w", encoding="utf-8", errors="ignore") as f:
                    for line in p.stdout:
                        f.write(line)
                        f.flush()
                        if "trycloudflare.com" in line:
                            idx = line.find("https://")
                            if idx >= 0:
                                end = line.find(" ", idx)
                                if end < 0:
                                    end = len(line)
                                self.tunnel_url = line[idx:end]
            except Exception:
                pass

        threading.Thread(target=reader, args=(proc,), daemon=True).start()

        deadline = time.time() + 30
        while time.time() < deadline:
            if self.tunnel_url:
                return {"ok": True, "url": self.tunnel_url}
            time.sleep(0.1)

        return {"ok": False, "error": "隧道启动超时或未解析到公网地址。"}

    def stop_tunnel(self):
        if self.tunnel_proc and self.tunnel_proc.poll() is None:
            try:
                self.tunnel_proc.terminate()
                self.tunnel_proc.wait(timeout=5)
            except Exception:
                self.tunnel_proc.kill()
            self.tunnel_proc = None
        self.tunnel_url = ''


def main():
    proc = start_server()
    try:
        if not wait_server():
            # 服务端子进程已经退出 → 基本可以断定是端口没抢到（luansha.exe 会打印 bind failed）。
            # 分开报错，避免把「端口被占用」误导成「启动慢」。
            if proc.poll() is not None:
                raise RuntimeError(
                    f"服务器没能启动：端口 {PORT} 已被其他程序占用。\n"
                    f"请关闭占用该端口的程序后重试（可在命令行执行 netstat -ano | findstr :{PORT} 查看占用者）。"
                )
            raise RuntimeError(f"服务器启动超时，请检查端口 {PORT} 是否被占用。")
        import webview
        api = Api()
        webview.create_window("乱杀跑团客户端", URL, width=1024, height=720, js_api=api)
        webview.start()
    finally:
        if proc and proc.poll() is None:
            proc.terminate()


if __name__ == "__main__":
    main()
