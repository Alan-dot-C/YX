// ============================================================================
//  YX —— 中枢（YX）
//
//  给 AI 用的观测层：把「电脑的真实」翻译成「AI 能消化的视图」。
//  同一个 exe 既是服务端也是客户端：
//
//    YX serve     常驻，监听命名管道
//    YX status    问常驻进程要状态；进程没开就在本地回一份
//
//  架构（见 docs\architecture.md）：
//    - 只有一个 worker 线程碰状态，io 线程只收发 —— 免锁，天然确定性
//    - 管道协议：u32 长度前缀 + UTF-8 JSON（不用换行分隔：JSON 里可能带换行）
//    - 读路径 fail-open：服务不在就本地算，绝不把调用方卡在 ReadFile 上
// ============================================================================
#include "../common.h"
#include "../log.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

#include <sddl.h>

#define YX_VERSION L"0.1.0"

// ------------------------------ 名字与路径 ------------------------------

static std::wstring UserName() {
    wchar_t buf[256] = {};
    DWORD   n = 256;
    if (!GetUserNameW(buf, &n)) return L"default";
    return buf;
}

// 管道名带用户名：一台机器上两个账户各跑一份时不会抢同一个管道。
static std::wstring PipeName() { return L"\\\\.\\pipe\\yx-" + UserName(); }
static std::wstring MutexName() { return L"Local\\yx-serve-" + UserName(); }

static std::wstring DataDir() {
    wchar_t* p = nullptr;
    size_t   n = 0;
    if (_wdupenv_s(&p, &n, L"LOCALAPPDATA") != 0 || !p) return L".\\yx";
    std::wstring d = p;
    free(p);
    return d + L"\\yx";
}

// ------------------------------ 全局状态 ------------------------------
//
// 只有 worker 线程碰队列里的任务；g_serving / g_clients 是原子量，
// 给 io 线程和退出流程用。除此之外不加锁。

struct Job {
    std::wstring req;
    std::wstring resp;
    HANDLE       ev = nullptr;
};

static std::deque<Job*>        g_q;
static std::mutex              g_qm;
static std::condition_variable g_qcv;
static std::atomic<bool>       g_serving{ true };
static std::atomic<int>        g_clients{ 0 };
static DWORD                   g_startTick = 0;

// ------------------------------ 极小的 JSON 取值 ------------------------------
//
// 只读我们自己客户端发的字段，不追求通用性：够用就好，
// 别为了几个字段把项目拖进一个 JSON 库（零依赖是硬约束）。

static std::wstring JsonField(const std::wstring& json, const wchar_t* key) {
    std::wstring pat = L"\"" + std::wstring(key) + L"\"";
    size_t p = json.find(pat);
    if (p == std::wstring::npos) return L"";
    p = json.find(L':', p + pat.size());
    if (p == std::wstring::npos) return L"";
    ++p;
    while (p < json.size() && (json[p] == L' ' || json[p] == L'\t' ||
                               json[p] == L'\r' || json[p] == L'\n')) ++p;
    if (p >= json.size()) return L"";
    if (json[p] == L'"') {
        size_t q = json.find(L'"', p + 1);
        if (q == std::wstring::npos) return L"";
        return json.substr(p + 1, q - p - 1);
    }
    size_t e = p;
    while (e < json.size() && json[e] != L',' && json[e] != L'}' &&
           json[e] != L' ' && json[e] != L'\r' && json[e] != L'\n') ++e;
    return json.substr(p, e - p);
}

// ------------------------------ 请求处理 ------------------------------
//
// 在 worker 线程里跑。以后 framing / zoom / memory / skill 都挂在这里，
// 因为它们的全部状态都在 worker 手上。

static std::wstring HandleOp(const std::wstring& req) {
    std::wstring op = JsonField(req, L"op");
    std::wstring id = JsonField(req, L"id");
    if (op.empty()) op = L"status";

    if (op == L"status") {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"id", id).Bool(L"ok", true).Str(L"op", L"status");
        d.Str(L"tool", L"yx").Str(L"version", YX_VERSION);
        d.Bool(L"server", true);
        d.Int(L"pid", (long long)GetCurrentProcessId());
        d.Int(L"uptime_ms", (long long)(GetTickCount() - g_startTick));
        d.Int(L"clients", g_clients.load());
        d.Str(L"pipe", PipeName());
        d.Str(L"data_dir", DataDir());
        return d.Emit();
    }

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"id", id).Bool(L"ok", false).Int(L"rc", 1);
    d.Str(L"error", L"unknown-op: " + op);
    return d.Emit();
}

static void WorkerLoop() {
    for (;;) {
        Job* j = nullptr;
        {
            std::unique_lock<std::mutex> lk(g_qm);
            g_qcv.wait(lk, [] { return !g_q.empty() || !g_serving.load(); });
            if (g_q.empty() && !g_serving.load()) return;
            j = g_q.front();
            g_q.pop_front();
        }
        // worker 是唯一执行者：这里出问题只影响这一个请求，不能让线程死掉
        j->resp = HandleOp(j->req);
        SetEvent(j->ev);
    }
}

// ------------------------------ 收发 ------------------------------
//
// 管道是字节流，ReadFile/WriteFile 都可能只走一部分，一律循环到齐。

static bool ReadExact(HANDLE h, void* buf, DWORD n) {
    BYTE* p = (BYTE*)buf;
    while (n > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, nullptr) || got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

static bool WriteExact(HANDLE h, const void* buf, DWORD n) {
    const BYTE* p = (const BYTE*)buf;
    while (n > 0) {
        DWORD put = 0;
        if (!WriteFile(h, p, n, &put, nullptr) || put == 0) return false;
        p += put;
        n -= put;
    }
    return true;
}

static void ServeConn(HANDLE h) {
    g_clients.fetch_add(1);
    for (;;) {
        uint32_t len = 0;
        if (!ReadExact(h, &len, sizeof(len)) || len == 0 || len > (1u << 20)) break;

        std::string body(len, '\0');
        if (!ReadExact(h, body.data(), len)) break;

        Job* j = new Job();
        j->req = FromUtf8(body);
        j->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!j->ev) { delete j; break; }

        {
            std::lock_guard<std::mutex> lk(g_qm);
            g_q.push_back(j);
        }
        g_qcv.notify_one();

        std::wstring resp;
        DWORD w = WaitForSingleObject(j->ev, 15000);
        if (w == WAIT_OBJECT_0) resp = j->resp;
        else resp = L"{\"v\":1,\"ok\":false,\"rc\":4,\"error\":\"worker-timeout\"}";
        CloseHandle(j->ev);
        delete j;

        std::string out = ToUtf8(resp);
        uint32_t n = (uint32_t)out.size();
        if (!WriteExact(h, &n, sizeof(n)) || !WriteExact(h, out.data(), n)) break;
    }
    g_clients.fetch_sub(1);
    FlushFileBuffers(h);
    DisconnectNamedPipe(h);
    CloseHandle(h);
}

// ------------------------------ 服务端 ------------------------------

static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_serving = false;
        // ConnectNamedPipe 是阻塞的，用一个假连接把它踢醒 —— 否则按了 Ctrl+C
        // 进程也要等下一个客户端连上来才肯退。
        HANDLE h = CreateFileW(PipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return TRUE;
    }
    return FALSE;
}

static int Serve() {
    // 单实例：同一用户只允许一份常驻。互斥体按 session 分（Local\），
    // 免得管理员权限启的那个和普通那个互相看不见，变成两个常驻。
    HANDLE once = CreateMutexW(nullptr, TRUE, MutexName().c_str());
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        Say(L"yx 已经在运行了（%ls）。要重启就先关掉那个。", PipeName().c_str());
        CloseHandle(once);
        return 6;
    }

    // 日志目录由 LogSetApp 决定（%LOCALAPPDATA%\yx\logs），这里不用再改。
    g_startTick = GetTickCount();

    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    // 只允许 SYSTEM 与管道所有者读写。不设这个，同一台机器上别的账户
    // 就能连进来，看到你的 AI 正在读什么。
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;OW)", SDDL_REVISION_1, &sd, nullptr);
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    std::thread worker(WorkerLoop);

    LogI(L"会话", 8000, L"会话", L"yx 服务启动",
         L"pipe=%ls pid=%lu version=%ls",
         PipeName().c_str(), (unsigned long)GetCurrentProcessId(), YX_VERSION);
    Say(L"yx listening on %ls", PipeName().c_str());

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    std::wstring name = PipeName();
    while (g_serving.load()) {
        HANDLE h = CreateNamedPipeW(name.c_str(),
                                    PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES,
                                    64 * 1024, 64 * 1024, 0, &sa);
        if (h == INVALID_HANDLE_VALUE) {
            LogE(L"会话", 2004, L"异常", L"创建管道失败",
                 L"err=%lu", (unsigned long)GetLastError());
            Sleep(200);
            continue;
        }
        BOOL connected = ConnectNamedPipe(h, nullptr);
        if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) connected = TRUE;
        if (connected && g_serving.load()) {
            std::thread(ServeConn, h).detach();
        } else {
            DisconnectNamedPipe(h);
            CloseHandle(h);
        }
    }

    // 叫醒 worker 让它收尾（排队里的请求照做完再退）
    g_qcv.notify_all();
    worker.join();

    if (sd) LocalFree(sd);
    LogI(L"会话", 8001, L"会话", L"yx 服务停止", L"ms=%lu",
         (unsigned long)(GetTickCount() - g_startTick));
    if (once) CloseHandle(once);
    return 0;
}

// ------------------------------ 客户端 ------------------------------

static bool CallServer(const std::wstring& req, std::wstring& resp, DWORD waitMs) {
    std::wstring name = PipeName();
    if (!WaitNamedPipeW(name.c_str(), waitMs)) return false;

    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string body = ToUtf8(req);
    uint32_t n = (uint32_t)body.size();
    bool ok = WriteExact(h, &n, sizeof(n)) && WriteExact(h, body.data(), n);

    if (ok) {
        uint32_t rn = 0;
        ok = ReadExact(h, &rn, sizeof(rn)) && rn > 0 && rn <= (1u << 20);
        if (ok) {
            std::string rb(rn, '\0');
            ok = ReadExact(h, rb.data(), rn);
            if (ok) resp = FromUtf8(rb);
        }
    }
    CloseHandle(h);
    return ok;
}

static int CmdStatus() {
    std::wstring req = L"{\"v\":1,\"op\":\"status\",\"id\":\"cli\"}";
    std::wstring resp;

    if (!CallServer(req, resp, 50)) {
        // 服务没开。本地回一份同构的 JSON，字段一样，只是 server=false。
        // 这就是 fail-open：yx 不在，调用方也不会卡死、也不用写两套分支。
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"id", L"cli").Bool(L"ok", true).Str(L"op", L"status");
        d.Str(L"tool", L"yx").Str(L"version", YX_VERSION);
        d.Bool(L"server", false);
        d.Int(L"pid", (long long)GetCurrentProcessId());
        d.Str(L"pipe", PipeName());
        d.Str(L"data_dir", DataDir());
        resp = d.Emit();
    }
    wprintf(L"%ls\n", resp.c_str());
    return 0;
}

// ------------------------------ 入口 ------------------------------

static void PrintUsage() {
    wprintf(L"YX —— 中枢（YX）v%ls\n", YX_VERSION);
    wprintf(L"给 AI 用的观测层：把电脑的真实翻译成 AI 能消化的视图。\n\n");
    wprintf(L"  yx serve     常驻，监听 %ls\n", PipeName().c_str());
    wprintf(L"  yx status    问常驻进程要状态（没开就在本地回一份）\n");
    wprintf(L"  yx help      这份帮助\n\n");
    wprintf(L"管道协议：u32 长度前缀 + UTF-8 JSON\n");
    wprintf(L"退出码：0 成功 / 1 参数错误 / 6 服务已在运行\n");
}

int wmain(int argc, wchar_t** argv) {
    InitConsoleUtf8();
    // 两个程序共用一个日志模块，账本要分开：配置目录、日志目录、锁名、文件名
    // 全部跟着程序名走。不这么做，客户端一跑就会往 handeye 的目录里写东西。
    LogSetApp(L"yx");
    LogInit();
    g_startTick = GetTickCount();

    std::wstring cmd = argc > 1 ? argv[1] : L"";
    for (wchar_t& c : cmd) c = towlower(c);

    int rc = 0;
    if (cmd.empty() || cmd == L"help" || cmd == L"--help" || cmd == L"-h") {
        PrintUsage();
    } else if (cmd == L"serve") {
        rc = Serve();
    } else if (cmd == L"status") {
        rc = CmdStatus();
    } else {
        Say(L"未知命令: %ls", cmd.c_str());
        PrintUsage();
        rc = 1;
    }

    LogShutdown();
    return rc;
}
