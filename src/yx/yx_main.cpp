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
#include "../eye.h"
#include "../sieve.h"
#include "../uia.h"
#include "../winenum.h"
#include "../input.h"
#include "../handeye_bridge.h"
#include "../schema.h"
#include "words.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <deque>
#include <map>
#include <set>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <cwctype>

#include <winrt/base.h>
#include <sddl.h>

#define YX_VERSION L"0.6.0"

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

// ------------------------------ 帧缓存与投影 ------------------------------
//
// 一次采集，三种投影。L0/L1/L2 都从这里出，zoom 只查缓存，不重新截屏。
// 所有状态都在 worker 线程手里，所以不需要锁。

struct CachedFrame {
    std::wstring ref;
    DWORD        at = 0;
    OcrResult    ocr;
    SieveResult  sieve;
    std::vector<UiNode> ui;
};

static std::map<std::wstring, CachedFrame> g_frames;
static std::map<std::wstring, std::wstring> g_lastRef;
static unsigned g_refSeq = 0;
static const size_t kMaxFrames = 16;

static std::wstring NextRef() {
    wchar_t buf[16] = {};
    swprintf_s(buf, L"%08x", ++g_refSeq);
    return buf;
}

static std::wstring RectJson(const RectI& r) {
    wchar_t buf[128] = {};
    swprintf_s(buf, L"{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", r.x, r.y, r.w, r.h);
    return buf;
}

static std::wstring LinesJson(const std::vector<OcrLine>& lines) {
    std::wstring s = L"[";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) s += L",";
        const OcrLine& l = lines[i];
        s += L"{\"text\":\"" + JsonStr(l.text) + L"\"";
        s += L",\"x\":" + std::to_wstring(l.box.x);
        s += L",\"y\":" + std::to_wstring(l.box.y);
        s += L",\"w\":" + std::to_wstring(l.box.w);
        s += L",\"h\":" + std::to_wstring(l.box.h);
        s += L",\"q\":" + std::to_wstring((int)(l.quality * 100.0));
        s += L",\"src\":\"" + JsonStr(l.src) + L"\"}";
    }
    s += L"]";
    return s;
}

static std::wstring UiJson(const std::vector<UiNode>& nodes) {
    std::wstring s = L"[";
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i) s += L",";
        const UiNode& n = nodes[i];
        s += L"{\"name\":\"" + JsonStr(n.name) + L"\"";
        s += L",\"role\":\"" + JsonStr(n.role) + L"\"";
        s += L",\"value\":\"" + JsonStr(n.value) + L"\"";
        s += L",\"x\":" + std::to_wstring(n.rect.x);
        s += L",\"y\":" + std::to_wstring(n.rect.y);
        s += L",\"w\":" + std::to_wstring(n.rect.w);
        s += L",\"h\":" + std::to_wstring(n.rect.h);
        s += n.invokable ? L",\"invokable\":true" : L",\"invokable\":false";
        s += L"}";
    }
    s += L"]";
    return s;
}

static std::wstring WindowsJson(const std::vector<WinInfo>& wins) {
    std::wstring s = L"[";
    for (size_t i = 0; i < wins.size(); ++i) {
        if (i) s += L",";
        const WinInfo& w = wins[i];
        s += L"{\"title\":\"" + JsonStr(w.title) + L"\"";
        s += L",\"x\":" + std::to_wstring(w.rect.x);
        s += L",\"y\":" + std::to_wstring(w.rect.y);
        s += L",\"w\":" + std::to_wstring(w.rect.w);
        s += L",\"h\":" + std::to_wstring(w.rect.h);
        s += L",\"z\":" + std::to_wstring(w.z);
        s += L"}";
    }
    s += L"]";
    return s;
}

static std::wstring AlarmsJson(const std::vector<std::wstring>& alarms) {
    std::wstring s = L"[";
    for (size_t i = 0; i < alarms.size(); ++i) {
        if (i) s += L",";
        s += L"\"" + JsonStr(alarms[i]) + L"\"";
    }
    s += L"]";
    return s;
}

static std::wstring Truncate(const std::wstring& s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + L"…";
}

static CachedFrame CaptureFrame() {
    CachedFrame f;
    f.ref = NextRef();
    f.at = GetTickCount();

    OcrRequest req;
    req.merge = true;
    req.padPx = 8;
    f.ocr = EyeRecognize(req);

    SieveOptions so;
    so.keepAll = true;       // 报警抽取必须在裁剪前完成
    so.doDelta = false;      // 变化由我们自己按 key 计算
    so.maxLines = 100000;
    so.maxLineChars = 100000;
    f.sieve = SieveRun(f.ocr.lines, so);

    if (UiInit()) {
        f.ui = UiTree(nullptr, 8, 600, 400);
    }
    return f;
}

static bool HasErrorWord(const std::wstring& text) {
    static const wchar_t* kWords[] = {
        L"错误", L"失败", L"警告", L"无法", L"拒绝", L"异常",
        L"ERROR", L"FAILED", L"DENIED", L"WARNING",
    };
    // 先去掉“无错误/没有异常/0 个错误”这种否定句，否则状态行会被当成报警。
    static const wchar_t* kNegations[] = {
        L"无错误", L"没有错误", L"无异常", L"没有异常",
        L"0 个错误", L"0个错误", L"无警告", L"NO ERROR", L"0 ERRORS",
    };
    std::wstring upper = ToUpper(text);
    for (const wchar_t* n : kNegations) ReplaceAll(upper, n, L"");
    for (const wchar_t* w : kWords) {
        if (upper.find(w) != std::wstring::npos) return true;
    }
    return false;
}

static std::vector<std::wstring> CollectAlarms(const CachedFrame& f) {
    std::vector<std::wstring> out;
    std::set<std::wstring> seen;

    auto add = [&](const std::wstring& s) {
        std::wstring t = Trim(s);
        if (t.empty() || !seen.insert(t).second) return;
        if (out.size() < 8) out.push_back(t);
    };

    for (const auto& b : f.sieve.blocks) {
        // 对话框本身就是报警：里面的控件全部保留
        if (b.kind == L"dialog") {
            for (const auto& it : b.items) add(it.text);
        }
        for (const auto& it : b.items) {
            if (HasErrorWord(it.text)) add(it.text);
        }
    }
    for (const auto& n : f.ui) {
        if (n.invokable && HasErrorWord(n.name)) add(n.name);
    }
    return out;
}

static int CountActions(const CachedFrame& f) {
    std::set<std::wstring> seen;
    for (const auto& n : f.ui) {
        if (!n.invokable || n.offscreen) continue;
        wchar_t key[320] = {};
        swprintf_s(key, L"%ls|%d,%d,%d,%d", n.name.c_str(),
                   n.rect.x, n.rect.y, n.rect.w, n.rect.h);
        seen.insert(key);
    }
    for (const auto& b : f.sieve.blocks) {
        if (b.kind != L"button") continue;
        for (const auto& it : b.items) {
            wchar_t key[320] = {};
            swprintf_s(key, L"%ls|%d,%d,%d,%d", it.text.c_str(),
                       it.box.x, it.box.y, it.box.w, it.box.h);
            seen.insert(key);
        }
    }
    return (int)seen.size();
}

static std::wstring FrameTitle(const CachedFrame& f) {
    for (const auto& w : f.sieve.windows) {
        if (!w.title.empty()) return w.title;
    }
    return GetForegroundTitle();
}

static std::wstring FrameType(const CachedFrame& f) {
    for (const auto& b : f.sieve.blocks) {
        if (b.kind == L"dialog") return L"对话框";
    }
    if (f.sieve.windows.empty()) return L"桌面";
    return L"窗口";
}

static std::wstring Zone(const RectI& r) {
    RectI scr = EyeScreenRect();
    if (scr.w <= 0 || scr.h <= 0) return L"未知";
    int cx = r.cx() * 3 / scr.w;
    int cy = r.cy() * 3 / scr.h;
    if (cx < 0) cx = 0;
    if (cx > 2) cx = 2;
    if (cy < 0) cy = 0;
    if (cy > 2) cy = 2;
    static const wchar_t* rows[] = { L"上", L"中", L"下" };
    static const wchar_t* cols[] = { L"左", L"中", L"右" };
    if (cy == 1 && cx == 1) return L"中央";
    return std::wstring(rows[cy]) + cols[cx];
}

static std::wstring BuildL0(const CachedFrame& f, const std::vector<std::wstring>& alarms,
                            const std::wstring& delta) {
    std::wstring type = FrameType(f);
    std::wstring title = Truncate(FrameTitle(f), 40);
    if (title.empty()) title = L"（无标题）";

    std::wstring summary;
    if (!f.ocr.ok) summary = L"看不清屏幕";
    else if (!alarms.empty()) summary = L"有 " + std::to_wstring(alarms.size()) + L" 条报警";
    else if (type == L"对话框") summary = L"对话框已打开";
    else summary = L"无错误";

    std::wstring out = L"[L0] " + type + L"「" + title + L"」| 可操作 " +
                       std::to_wstring(CountActions(f)) + L" 项 | " + summary;
    if (delta.find(L"\"changed\":true") != std::wstring::npos) out += L" | 有变化";

    for (size_t i = 0; i < alarms.size() && i < 2; ++i) {
        out += L"\n[报警] " + Truncate(alarms[i], 80);
    }
    if (alarms.size() > 2) {
        out += L"\n[报警] +" + std::to_wstring(alarms.size() - 2);
    }
    return out;
}

static std::wstring BuildL1(const CachedFrame& f, const std::vector<std::wstring>& alarms) {
    std::wstring out = L"[L1] " + FrameType(f) + L"「" + Truncate(FrameTitle(f), 40) + L"」";
    int lines = 1;

    for (const auto& b : f.sieve.blocks) {
        if (b.items.empty()) continue;
        std::wstring text = b.items[0].text;
        out += L"\n[" + b.kind + L"] " + Truncate(text, 60) + L" @" + Zone(b.box);
        if (++lines >= 8) break;
    }
    for (const auto& n : f.ui) {
        if (!n.invokable || n.offscreen) continue;
        out += L"\n[控件] " + n.role + L":" + Truncate(n.name, 40) + L" @" + Zone(n.rect);
        if (++lines >= 10) break;
    }
    for (size_t i = 0; i < alarms.size() && i < 2; ++i) {
        out += L"\n[报警] " + Truncate(alarms[i], 80);
    }
    return out;
}

static std::wstring DeltaJson(const CachedFrame* prev, const CachedFrame& cur) {
    int added = 0;
    int removed = 0;
    if (prev) {
        std::set<std::wstring> a;
        std::set<std::wstring> b;
        for (const auto& l : prev->ocr.lines) {
            std::wstring t = ToUpper(Trim(l.text));
            if (!t.empty()) a.insert(t);
        }
        for (const auto& l : cur.ocr.lines) {
            std::wstring t = ToUpper(Trim(l.text));
            if (!t.empty()) b.insert(t);
        }
        for (const auto& s : b) if (!a.count(s)) ++added;
        for (const auto& s : a) if (!b.count(s)) ++removed;
    }
    wchar_t buf[160] = {};
    swprintf_s(buf, L"{\"added\":%d,\"removed\":%d,\"changed\":%ls}",
               added, removed, (added > 0 || removed > 0) ? L"true" : L"false");
    return buf;
}

// ------------------------------ 请求处理 ------------------------------
//
// 在 worker 线程里跑。以后 framing / zoom / memory / skill 都挂在这里，
// 因为它们的全部状态都在 worker 手上。

static std::wstring ErrorResponse(const std::wstring& id, const std::wstring& op,
                                  int rc, const std::wstring& error) {
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"id", id).Bool(L"ok", false).Str(L"op", op);
    d.Int(L"rc", rc).Str(L"error", error);
    return d.Emit();
}

static std::wstring FrameResponse(const std::wstring& id, const std::wstring& op,
                                  const CachedFrame& f, const std::wstring& level,
                                  const std::vector<std::wstring>& alarms,
                                  const std::wstring& delta, DWORD ms) {
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"id", id).Bool(L"ok", true).Str(L"op", op);
    d.Str(L"level", level).Str(L"ref", f.ref);
    d.Bool(L"cached", op == L"zoom");

    std::wstring text;
    if (level == L"L2") {
        text = L"[L2] ref=" + f.ref + L" raw=" + std::to_wstring(f.ocr.lines.size()) +
               L" ui=" + std::to_wstring(f.ui.size());
    } else if (level == L"L1") {
        text = BuildL1(f, alarms);
    } else {
        text = BuildL0(f, alarms, delta);
    }
    d.Str(L"text", text);
    d.Raw(L"delta", delta);
    d.Raw(L"alarms", AlarmsJson(alarms));
    d.Int(L"line_count", (long long)f.ocr.lines.size());
    d.Int(L"ui_nodes", (long long)f.ui.size());
    d.Int(L"ms", (long long)ms);

    if (level == L"L2") {
        d.Raw(L"lines", LinesJson(f.ocr.lines));
        d.Raw(L"ui", UiJson(f.ui));
        d.Raw(L"windows", WindowsJson(f.sieve.windows));
    }
    return d.Emit();
}

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
        d.Int(L"frames", (long long)g_frames.size());
        d.Str(L"pipe", PipeName());
        d.Str(L"data_dir", DataDir());
        return d.Emit();
    }

    if (op == L"framing") {
        std::wstring key = JsonField(req, L"key");
        if (key.empty()) key = L"default";
        std::wstring target = JsonField(req, L"target");
        if (!target.empty() && target != L"screen") {
            return ErrorResponse(id, op, 1, L"unsupported target: " + target);
        }
        std::wstring level = ToUpper(JsonField(req, L"level"));
        if (level.empty() || level == L"AUTO") level = L"L0";
        if (level != L"L0" && level != L"L1" && level != L"L2") {
            return ErrorResponse(id, op, 1, L"bad level: " + level);
        }

        DWORD t0 = GetTickCount();
        CachedFrame f = CaptureFrame();
        if (!f.ocr.ok) {
            return ErrorResponse(id, op, 3, f.ocr.error.empty() ? L"capture failed" : f.ocr.error);
        }

        std::vector<std::wstring> alarms = CollectAlarms(f);

        const CachedFrame* prev = nullptr;
        auto last = g_lastRef.find(key);
        if (last != g_lastRef.end()) {
            auto it = g_frames.find(last->second);
            if (it != g_frames.end()) prev = &it->second;
        }
        std::wstring delta = DeltaJson(prev, f);

        if (g_frames.size() >= kMaxFrames) {
            auto oldest = g_frames.begin();
            for (auto it = g_frames.begin(); it != g_frames.end(); ++it) {
                if (it->second.at < oldest->second.at) oldest = it;
            }
            g_frames.erase(oldest);
        }
        DWORD ms = GetTickCount() - t0;
        g_frames[f.ref] = f;
        g_lastRef[key] = f.ref;
        return FrameResponse(id, op, f, level, alarms, delta, ms);
    }

    if (op == L"zoom") {
        std::wstring ref = JsonField(req, L"ref");
        auto it = g_frames.find(ref);
        if (it == g_frames.end()) {
            return ErrorResponse(id, op, 4, L"ref not found or expired: " + ref);
        }
        const CachedFrame& f = it->second;
        std::vector<std::wstring> alarms = CollectAlarms(f);
        std::wstring delta = L"{\"added\":0,\"removed\":0,\"changed\":false}";
        return FrameResponse(id, op, f, L"L2", alarms, delta, 0);
    }

    return ErrorResponse(id, op, 1, L"unknown-op: " + op);
}

static void WorkerLoop() {
    // OCR / UIA 都是 COM；worker 是唯一碰采集的线程，所以它自己进 MTA。
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        // 已初始化过就忽略
    }

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

static std::wstring GetOpt(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    }
    return L"";
}

static int CmdFraming(int argc, wchar_t** argv) {
    std::wstring key = GetOpt(argc, argv, L"--key");
    std::wstring level = GetOpt(argc, argv, L"--level");
    if (key.empty()) key = L"default";
    if (level.empty()) level = L"auto";

    std::wstring req = L"{\"v\":1,\"op\":\"framing\",\"id\":\"cli\",\"key\":\"" +
                       JsonStr(key) + L"\",\"target\":\"screen\",\"level\":\"" +
                       JsonStr(level) + L"\"}";
    std::wstring resp;
    if (CallServer(req, resp, 50)) {
        wprintf(L"%ls\n", resp.c_str());
        return 0;
    }

    // fail-open：服务不在，本地采集并投影，只是没有跨调用状态。
    DWORD t0 = GetTickCount();
    CachedFrame f = CaptureFrame();
    if (!f.ocr.ok) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"id", L"cli").Bool(L"ok", false).Str(L"op", L"framing");
        d.Int(L"rc", 3).Str(L"error", f.ocr.error.empty() ? L"capture failed" : f.ocr.error);
        wprintf(L"%ls\n", d.Emit().c_str());
        return 3;
    }
    std::wstring lv = ToUpper(level);
    if (lv.empty() || lv == L"AUTO") lv = L"L0";
    if (lv != L"L0" && lv != L"L1" && lv != L"L2") lv = L"L0";

    auto alarms = CollectAlarms(f);
    std::wstring delta = L"{\"added\":0,\"removed\":0,\"changed\":false}";
    DWORD ms = GetTickCount() - t0;
    wprintf(L"%ls\n", FrameResponse(L"cli", L"framing", f, lv, alarms, delta, ms).c_str());
    return 0;
}

static int CmdZoom(int argc, wchar_t** argv) {
    std::wstring ref = GetOpt(argc, argv, L"--ref");
    if (ref.empty() && argc > 2 && argv[2][0] != L'-') ref = argv[2];
    if (ref.empty()) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"id", L"cli").Bool(L"ok", false).Str(L"op", L"zoom");
        d.Int(L"rc", 1).Str(L"error", L"missing --ref");
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }
    std::wstring req = L"{\"v\":1,\"op\":\"zoom\",\"id\":\"cli\",\"ref\":\"" +
                       JsonStr(ref) + L"\"}";
    std::wstring resp;
    if (!CallServer(req, resp, 50)) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"id", L"cli").Bool(L"ok", false).Str(L"op", L"zoom");
        d.Int(L"rc", 6).Str(L"error", L"server not running");
        wprintf(L"%ls\n", d.Emit().c_str());
        return 6;
    }
    wprintf(L"%ls\n", resp.c_str());
    return 0;
}

static int CmdLogWrite(int argc, wchar_t** argv) {
    std::wstring dir = GetOpt(argc, argv, L"--dir");
    if (!dir.empty()) LogConfigure(true, dir, false, 0, false, 0, false, true);

    LogI(L"测试", 9001, L"测试", L"测试事件", L"n=%d", 1);

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"log.write").Bool(L"ok", true);
    d.Str(L"path", LogFilePath());
    wprintf(L"%ls\n", d.Emit().c_str());
    return 0;
}

static int CmdLogVerify(int argc, wchar_t** argv) {
    std::wstring dir = GetOpt(argc, argv, L"--dir");
    if (!dir.empty()) LogConfigure(true, dir, false, 0, false, 0, false, true);

    LogVerifyResult r = LogVerify();

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"log.verify").Bool(L"ok", r.ok);
    d.Int(L"chained", r.chained).Int(L"unverified", r.unverified).Bool(L"head_ok", r.headOk);
    if (!r.ok) {
        d.Int(L"rc", 5).Str(L"error", r.error);
        if (!r.file.empty()) d.Str(L"file", r.file);
        if (r.line > 0) d.Int(L"line", r.line);
        if (!r.expected.empty()) d.Str(L"expected", r.expected);
        if (!r.actual.empty()) d.Str(L"actual", r.actual);
    }
    wprintf(L"%ls\n", d.Emit().c_str());
    return r.ok ? 0 : 5;
}

static int CmdLog(int argc, wchar_t** argv) {
    std::wstring sub = argc > 2 ? argv[2] : L"verify";
    sub = ToUpper(sub);
    if (sub == L"WRITE") return CmdLogWrite(argc, argv);
    if (sub == L"VERIFY" || sub.empty()) return CmdLogVerify(argc, argv);

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"log").Bool(L"ok", false);
    d.Int(L"rc", 1).Str(L"error", L"unknown subcommand: " + sub);
    wprintf(L"%ls\n", d.Emit().c_str());
    return 1;
}



static bool GetFlag(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 2; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return true;
    }
    return false;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::string ReadFileLimited(const std::wstring& path, size_t maxBytes, bool& truncated) {
    truncated = false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    size_t take = (size_t)sz;
    if (take > maxBytes) { take = maxBytes; truncated = true; }
    std::string out(take, '\0');
    size_t got = take ? fread(&out[0], 1, take, f) : 0;
    out.resize(got);
    fclose(f);
    return out;
}

// cmd.exe 有的程序输出 UTF-8，有的仍然按 OEM 代码页输出（中文 Windows 是 936）。
// 先按 UTF-8 严格解码；失败再按 OEM 解一次。
static std::wstring DecodeConsoleBytes(const std::string& bytes) {
    if (bytes.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                (int)bytes.size(), nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) {
        cp = CP_OEMCP;
        n = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    }
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), &out[0], n);
    return out;
}

static bool RunProcessCapture(const std::wstring& cmdLine, DWORD timeoutMs,
                              std::string& out, std::string& err,
                              DWORD& exitCode, bool& timedOut, bool& truncated) {
    out.clear();
    err.clear();
    exitCode = 1;
    timedOut = false;
    truncated = false;

    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return false;
    std::wstring base = std::wstring(tmp) + L"yx-" + std::to_wstring(GetCurrentProcessId()) +
                        L"-" + std::to_wstring(GetTickCount());
    std::wstring outPath = base + L"-out.txt";
    std::wstring errPath = base + L"-err.txt";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = CreateFileW(outPath.c_str(), GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    HANDLE hErr = CreateFileW(errPath.c_str(), GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        DeleteFileW(outPath.c_str());
        DeleteFileW(errPath.c_str());
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNul;
    si.hStdOutput = hOut;
    si.hStdError = hErr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(0);

    BOOL created = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOut);
    CloseHandle(hErr);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    if (!created) {
        DeleteFileW(outPath.c_str());
        DeleteFileW(errPath.c_str());
        err = "CreateProcess failed";
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    bool t1 = false;
    bool t2 = false;
    out = ReadFileLimited(outPath, 64 * 1024, t1);
    err = ReadFileLimited(errPath, 64 * 1024, t2);
    truncated = t1 || t2;
    DeleteFileW(outPath.c_str());
    DeleteFileW(errPath.c_str());
    return true;
}

static bool RunHandeyeOpen(const Word& w, DWORD& exitCode, std::string& out,
                           std::string& err, bool& timedOut, bool& truncated) {
    out.clear();
    err.clear();
    timedOut = false;
    truncated = false;
    std::wstring extra = (w.args && w.args[0]) ? std::wstring(w.args) : L"";
    exitCode = (DWORD)HandeyeOpen(w.target, extra);
    return true;
}

static std::wstring Compact(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (!iswspace(c)) out += (wchar_t)towlower(c);
    }
    return out;
}

// 输入模糊化：先精确名字，再精确别名/描述，最后唯一子串。
// 匹配不到或有歧义都返回 nullptr；matches 可选带回候选。
static const Word* ResolveWord(const std::wstring& query,
                               std::vector<const Word*>* matches = nullptr) {
    std::wstring q = ToUpper(Trim(query));
    if (q.empty()) return nullptr;

    for (const auto& w : AllWords()) {
        if (ToUpper(w.name) == q) return &w;
    }
    for (const auto& w : AllWords()) {
        if (w.alias) {
            for (const auto& a : Split(w.alias, L'|')) {
                if (ToUpper(Trim(a)) == q) return &w;
            }
        }
    }
    for (const auto& w : AllWords()) {
        if (ToUpper(w.desc) == q) return &w;
    }

    std::vector<const Word*> hits;
    for (const auto& w : AllWords()) {
        std::wstring hay = ToUpper(std::wstring(w.name) + L" " + w.desc +
                                   (w.alias ? (L" " + std::wstring(w.alias)) : L""));
        if (hay.find(q) != std::wstring::npos) hits.push_back(&w);
    }
    if (hits.size() == 1) return hits[0];
    if (matches) *matches = hits;
    return nullptr;
}

static bool CaptureForTask(const std::wstring& image, OcrResult& r) {
    if (!image.empty()) {
        r = EyeRecognizeImageFile(image);
        return r.ok;
    }
    OcrRequest req;
    req.merge = true;
    req.padPx = 8;
    r = EyeRecognize(req);
    return r.ok;
}

static int CmdOpenQuery(int argc, wchar_t** argv) {
    std::wstring query = (argc > 2 && argv[2][0] != L'-') ? argv[2] : GetOpt(argc, argv, L"--target");
    if (query.empty()) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"open").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"error", L"missing target");
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }

    auto direct = [&] {
        if (query.find(L':') != std::wstring::npos) return true;
        if (query.find(L'\\') != std::wstring::npos || query.find(L'/') != std::wstring::npos) return true;
        std::wstring low = ToUpper(query);
        for (const wchar_t* ext : { L".EXE", L".MSC", L".CPL", L".BAT", L".COM" }) {
            if (low.size() >= wcslen(ext) && low.compare(low.size() - wcslen(ext), wcslen(ext), ext) == 0)
                return true;
        }
        return false;
    };

    if (direct()) {
        int rc = HandeyeOpen(query, L"");
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"open").Bool(L"ok", rc == 0);
        d.Str(L"target", query);
        if (rc != 0) d.Int(L"rc", rc);
        wprintf(L"%ls\n", d.Emit().c_str());
        return rc;
    }

    std::vector<const Word*> cands;
    const Word* w = ResolveWord(query, &cands);
    if (!w) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"open").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"error", cands.empty() ? (L"unknown: " + query)
                                                     : (L"ambiguous: " + query));
        if (!cands.empty()) {
            std::wstring s = L"[";
            for (size_t i = 0; i < cands.size(); ++i) {
                if (i) s += L",";
                s += L"\"" + JsonStr(cands[i]->name) + L"\"";
            }
            s += L"]";
            d.Raw(L"candidates", s);
        }
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }
    if (w->kind != WordKind::Open) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"open").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"error", std::wstring(L"use: yx run ") + w->name);
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }

    int rc = HandeyeOpen(w->target, w->args ? w->args : L"");
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"open").Bool(L"ok", rc == 0);
    d.Str(L"query", query).Str(L"name", w->name).Str(L"target", w->target);
    if (rc != 0) d.Int(L"rc", rc);
    wprintf(L"%ls\n", d.Emit().c_str());
    return rc;
}

// ------------------------------ 重复记忆 ------------------------------
//
// 记录 AI 反复执行的动作；同一指纹第 5 次成功时进入 proposed。
// 人工用 yx words accept 确认后，写入 words.user.txt，下次只发一个词。
// 这里不做语义理解，只按“动作 + 规范化参数 + 图片”做指纹。

struct RepeatItem {
    std::wstring fp;
    int          count = 0;
    std::wstring state;      // observing / proposed / active / rejected
    std::wstring action;
    std::wstring arg;
    std::wstring image;
};

struct UserWord {
    std::wstring name;
    std::wstring action;
    std::wstring arg;
    std::wstring image;
};

static std::wstring RepeatPath() { return DataDir() + L"\\repeat.txt"; }
static std::wstring UserWordsPath() { return DataDir() + L"\\words.user.txt"; }

static bool ReadTextFile(const std::wstring& path, std::wstring& out) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        std::string bytes(size_t(sz), '\0');
        size_t got = fread(&bytes[0], 1, size_t(sz), f);
        bytes.resize(got);
        out = FromUtf8(bytes);
    }
    fclose(f);
    return true;
}

static bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    std::string bytes = ToUtf8(text);
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    return true;
}

static std::wstring StripCr(std::wstring s) {
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

static std::vector<RepeatItem> LoadRepeat() {
    std::vector<RepeatItem> out;
    std::wstring text;
    if (!ReadTextFile(RepeatPath(), text)) return out;
    for (auto& line : Split(text, L'\n')) {
        auto p = Split(StripCr(line), L'\t');
        if (p.size() < 6) continue;
        RepeatItem it;
        it.fp = p[0];
        it.count = _wtoi(p[1].c_str());
        it.state = p[2];
        it.action = p[3];
        it.arg = p[4];
        it.image = StripCr(p[5]);
        out.push_back(std::move(it));
    }
    return out;
}

static void SaveRepeat(const std::vector<RepeatItem>& items) {
    std::wstring text;
    for (const auto& it : items) {
        text += it.fp + L"\t" + std::to_wstring(it.count) + L"\t" + it.state + L"\t" +
                it.action + L"\t" + it.arg + L"\t" + it.image + L"\r\n";
    }
    WriteTextFile(RepeatPath(), text);
}

static std::wstring FnvHex(const std::wstring& s) {
    uint64_t h = 0xCBF29CE484222325ULL;
    std::string bytes = ToUtf8(s);
    for (unsigned char c : bytes) {
        h ^= (uint64_t)c;
        h *= 0x100000001B3ULL;
    }
    wchar_t buf[32] = {};
    swprintf_s(buf, L"%016llX", (unsigned long long)h);
    return buf;
}

static std::wstring RecordRepeat(const std::wstring& action, const std::wstring& arg,
                                 const std::wstring& image) {
    if (arg.empty()) return L"";

    HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\yx_repeat_mutex");
    if (m) WaitForSingleObject(m, 2000);

    auto items = LoadRepeat();
    std::wstring key = action + L"|" + Compact(arg) + L"|" + image;
    std::wstring fp = FnvHex(key);

    RepeatItem* hit = nullptr;
    for (auto& it : items) {
        if (it.fp == fp) { hit = &it; break; }
    }
    if (!hit) {
        RepeatItem it;
        it.fp = fp;
        it.state = L"observing";
        it.action = action;
        it.arg = arg;
        it.image = image;
        items.push_back(it);
        hit = &items.back();
    }

    hit->count++;
    std::wstring learn;
    if (hit->count >= 5 && hit->state == L"observing") {
        hit->state = L"proposed";
        learn = L"{\"fingerprint\":\"" + fp + L"\",\"count\":" +
                std::to_wstring(hit->count) + L",\"state\":\"proposed\"}";
        fwprintf(stderr, L"[YX] 新词提议：%ls %ls（第 %d 次）\n",
                 action.c_str(), arg.c_str(), hit->count);
    } else if (hit->count >= 5 && hit->state == L"proposed") {
        learn = L"{\"fingerprint\":\"" + fp + L"\",\"count\":" +
                std::to_wstring(hit->count) + L",\"state\":\"proposed\"}";
    }

    SaveRepeat(items);
    if (m) { ReleaseMutex(m); CloseHandle(m); }
    return learn;
}

static std::vector<UserWord> LoadUserWords() {
    std::vector<UserWord> out;
    std::wstring text;
    if (!ReadTextFile(UserWordsPath(), text)) return out;
    for (auto& line : Split(text, L'\n')) {
        auto p = Split(StripCr(line), L'\t');
        if (p.size() < 4) continue;
        UserWord w;
        w.name = p[0];
        w.action = p[1];
        w.arg = p[2];
        w.image = StripCr(p[3]);
        out.push_back(std::move(w));
    }
    return out;
}

static const UserWord* FindUserWord(const std::wstring& name) {
    static std::vector<UserWord> cache;
    cache = LoadUserWords();
    for (const auto& w : cache) {
        if (_wcsicmp(w.name.c_str(), name.c_str()) == 0) return &w;
    }
    return nullptr;
}

static bool ValidWordName(const std::wstring& name) {
    if (name.empty()) return false;
    for (wchar_t c : name) {
        if (!(iswalnum(c) || c == L'_' || c == L'-')) return false;
    }
    return true;
}

static bool SaveUserWord(const UserWord& w) {
    auto items = LoadUserWords();
    for (const auto& it : items) {
        if (_wcsicmp(it.name.c_str(), w.name.c_str()) == 0) return false;
    }
    std::wstring text;
    ReadTextFile(UserWordsPath(), text);
    if (!text.empty() && text.back() != L'\n') text += L"\r\n";
    text += w.name + L"\t" + w.action + L"\t" + w.arg + L"\t" + w.image + L"\r\n";
    return WriteTextFile(UserWordsPath(), text);
}

static int CmdFind(int argc, wchar_t** argv) {
    std::wstring needle = (argc > 2 && argv[2][0] != L'-') ? argv[2] : GetOpt(argc, argv, L"--text");
    if (needle.empty()) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"find").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"error", L"missing text");
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }
    std::wstring image = GetOpt(argc, argv, L"--image");
    std::wstring matches = L"[";
    int count = 0;
    std::wstring via;

    if (image.empty() && UiInit()) {
        auto nodes = UiFind(needle, 300, 400);
        for (const auto& n : nodes) {
            if (!n.enabled || n.offscreen || n.rect.w <= 0) continue;
            if (count++) matches += L",";
            matches += L"{\"text\":\"" + JsonStr(n.name) + L"\"";
            matches += L",\"role\":\"" + JsonStr(n.role) + L"\"";
            matches += L",\"x\":" + std::to_wstring(n.rect.x);
            matches += L",\"y\":" + std::to_wstring(n.rect.y);
            matches += L",\"w\":" + std::to_wstring(n.rect.w);
            matches += L",\"h\":" + std::to_wstring(n.rect.h);
            matches += L",\"score\":1.0,\"via\":\"ui\"}";
            if (count >= 8) break;
        }
        if (count) via = L"ui";
    }

    if (count == 0) {
        OcrResult r;
        if (!CaptureForTask(image, r)) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"find").Bool(L"ok", false);
            d.Int(L"rc", 3).Str(L"error", r.error.empty() ? L"capture failed" : r.error);
            wprintf(L"%ls\n", d.Emit().c_str());
            return 3;
        }
        auto hits = SieveFind(r.lines, needle, 8);
        for (const auto& h : hits) {
            if (count++) matches += L",";
            matches += L"{\"text\":\"" + JsonStr(h.text) + L"\"";
            matches += L",\"x\":" + std::to_wstring(h.box.x);
            matches += L",\"y\":" + std::to_wstring(h.box.y);
            matches += L",\"w\":" + std::to_wstring(h.box.w);
            matches += L",\"h\":" + std::to_wstring(h.box.h);
            wchar_t buf[32] = {};
            swprintf_s(buf, L"%.3f", h.score);
            matches += L",\"score\":" + std::wstring(buf) + L",\"via\":\"ocr\"}";
        }
        via = L"ocr";
    }
    matches += L"]";

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"find").Bool(L"ok", count > 0);
    d.Str(L"text", needle).Str(L"via", via).Int(L"count", count);
    d.Raw(L"matches", matches);
    wprintf(L"%ls\n", d.Emit().c_str());
    return count > 0 ? 0 : 1;
}

static int CmdRead(int argc, wchar_t** argv) {
    std::wstring needle = GetOpt(argc, argv, L"--find");
    std::wstring image = GetOpt(argc, argv, L"--image");
    int context = _wtoi(GetOpt(argc, argv, L"--context").c_str());
    if (context < 0) context = 0;
    if (context == 0 && GetOpt(argc, argv, L"--context").empty()) context = 1;

    OcrResult r;
    if (!CaptureForTask(image, r)) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"read").Bool(L"ok", false);
        d.Int(L"rc", 3).Str(L"error", r.error.empty() ? L"capture failed" : r.error);
        wprintf(L"%ls\n", d.Emit().c_str());
        return 3;
    }

    std::vector<size_t> idx;
    if (needle.empty()) {
        for (size_t i = 0; i < r.lines.size() && i < 60; ++i) idx.push_back(i);
    } else {
        std::wstring n = Compact(needle);
        for (size_t i = 0; i < r.lines.size(); ++i) {
            if (Compact(r.lines[i].text).find(n) != std::wstring::npos) idx.push_back(i);
        }
    }

    std::set<size_t> take;
    if (needle.empty()) {
        for (size_t i : idx) take.insert(i);
    } else {
        for (size_t i : idx) {
            const RectI& m = r.lines[i].box;
            int top = m.y - m.h * (context + 2);
            int bot = m.bottom() + m.h * (context + 2);
            for (size_t j = 0; j < r.lines.size(); ++j) {
                const RectI& b = r.lines[j].box;
                int left = (std::max)(m.x, b.x);
                int right = (std::min)(m.right(), b.right());
                if (j == i || (b.cy() >= top && b.cy() <= bot && right > left)) {
                    take.insert(j);
                }
            }
        }
    }

    std::vector<size_t> ordered(take.begin(), take.end());
    std::sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
        if (r.lines[a].box.y != r.lines[b].box.y) return r.lines[a].box.y < r.lines[b].box.y;
        return r.lines[a].box.x < r.lines[b].box.x;
    });

    std::wstring lines = L"[";
    std::wstring text;
    int count = 0;
    for (size_t i : ordered) {
        if (count++) lines += L",";
        const OcrLine& l = r.lines[i];
        lines += L"{\"text\":\"" + JsonStr(l.text) + L"\"";
        lines += L",\"x\":" + std::to_wstring(l.box.x);
        lines += L",\"y\":" + std::to_wstring(l.box.y);
        lines += L",\"w\":" + std::to_wstring(l.box.w);
        lines += L",\"h\":" + std::to_wstring(l.box.h) + L"}";
        if (!text.empty()) text += L"\n";
        text += l.text;
    }
    lines += L"]";

    std::wstring learn = (count > 0 && !needle.empty())
                             ? RecordRepeat(L"read", needle, image)
                             : L"";

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"read").Bool(L"ok", count > 0);
    d.Str(L"find", needle).Str(L"text", text).Int(L"count", count);
    d.Raw(L"lines", lines);
    if (!learn.empty()) d.Raw(L"learn", learn);
    wprintf(L"%ls\n", d.Emit().c_str());
    return count > 0 ? 0 : 1;
}

static bool ScreenHasText(const std::wstring& needle) {
    OcrRequest req;
    req.merge = true;
    OcrResult r = EyeRecognize(req);
    return r.ok && !SieveFind(r.lines, needle, 1).empty();
}

static int VerifyExpect(const std::wstring& expectText, const std::wstring& expectGone, int timeoutMs) {
    if (expectText.empty() && expectGone.empty()) return 0;
    if (timeoutMs <= 0) timeoutMs = 3000;
    DWORD start = GetTickCount();
    for (;;) {
        bool ok = true;
        if (!expectText.empty()) ok = ok && ScreenHasText(expectText);
        if (!expectGone.empty()) ok = ok && !ScreenHasText(expectGone);
        if (ok) return 0;
        if ((int)(GetTickCount() - start) > timeoutMs) return 5;
        Sleep(250);
    }
}

static int CmdClickText(int argc, wchar_t** argv) {
    std::wstring needle = (argc > 2 && argv[2][0] != L'-') ? argv[2] : GetOpt(argc, argv, L"--text");
    if (needle.empty()) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"click-text").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"error", L"missing text");
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }
    int index = _wtoi(GetOpt(argc, argv, L"--index").c_str());
    bool right = GetFlag(argc, argv, L"--right");
    bool dry = GetFlag(argc, argv, L"--dry-run");
    std::wstring image = GetOpt(argc, argv, L"--image");
    std::wstring expectText = GetOpt(argc, argv, L"--expect");
    std::wstring expectGone = GetOpt(argc, argv, L"--expect-gone");
    int expectTimeout = _wtoi(GetOpt(argc, argv, L"--expect-timeout").c_str());

    int x = 0, y = 0;
    bool found = false;
    std::wstring via;

    if (image.empty() && UiInit()) {
        auto nodes = UiFind(needle, 300, 400);
        std::vector<UiNode> usable;
        for (const auto& n : nodes) {
            if (n.enabled && !n.offscreen && n.rect.w > 0) usable.push_back(n);
        }
        if (!usable.empty()) {
            if (index < 0 || index >= (int)usable.size()) index = 0;
            const UiNode& n = usable[index];
            x = n.rect.cx();
            y = n.rect.cy();
            found = true;
            via = L"ui";
            if (!dry) {
                InputInit();
                if (!(n.invokable && UiInvoke(n))) {
                    MoveTo(x, y, 200);
                    Sleep(60);
                    ClickNow(right);
                }
                InputShutdown();
            }
        }
    }

    if (!found) {
        OcrResult r;
        if (!CaptureForTask(image, r)) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"click-text").Bool(L"ok", false);
            d.Int(L"rc", 3).Str(L"error", r.error.empty() ? L"capture failed" : r.error);
            wprintf(L"%ls\n", d.Emit().c_str());
            return 3;
        }
        auto hits = SieveFind(r.lines, needle, 8);
        if (!hits.empty()) {
            if (index < 0 || index >= (int)hits.size()) index = 0;
            x = hits[index].box.cx();
            y = hits[index].box.cy();
            found = true;
            via = L"ocr";
            if (!dry && image.empty()) {
                InputInit();
                MoveTo(x, y, 200);
                Sleep(60);
                ClickNow(right);
                InputShutdown();
            }
        }
    }

    if (!found) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"click-text").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"text", needle).Str(L"error", L"not found");
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }

    int verifyRc = 0;
    if (!dry) verifyRc = VerifyExpect(expectText, expectGone, expectTimeout);

    std::wstring learn = (!dry && verifyRc == 0)
                             ? RecordRepeat(L"click-text", needle, L"")
                             : L"";

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"click-text");
    d.Bool(L"ok", verifyRc == 0).Str(L"text", needle).Str(L"via", via);
    d.Int(L"x", x).Int(L"y", y).Bool(L"clicked", !dry && image.empty());
    if (!learn.empty()) d.Raw(L"learn", learn);
    if (verifyRc != 0) d.Int(L"rc", verifyRc).Str(L"error", L"expect failed");
    wprintf(L"%ls\n", d.Emit().c_str());
    return verifyRc;
}

static int CmdWait(int argc, wchar_t** argv) {
    std::wstring needle = (argc > 2 && argv[2][0] != L'-') ? argv[2] : GetOpt(argc, argv, L"--text");
    if (needle.empty()) {
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"wait").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"error", L"missing text");
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }
    bool gone = GetFlag(argc, argv, L"--gone");
    int timeout = _wtoi(GetOpt(argc, argv, L"--timeout").c_str());
    if (timeout <= 0) timeout = 5000;

    DWORD start = GetTickCount();
    for (;;) {
        bool found = ScreenHasText(needle);
        if (gone ? !found : found) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"wait").Bool(L"ok", true);
            d.Str(L"text", needle).Bool(L"gone", gone);
            d.Int(L"ms", (long long)(GetTickCount() - start));
            wprintf(L"%ls\n", d.Emit().c_str());
            return 0;
        }
        if ((int)(GetTickCount() - start) > timeout) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"wait").Bool(L"ok", false);
            d.Str(L"text", needle).Bool(L"gone", gone);
            d.Int(L"rc", 4).Str(L"error", L"timeout");
            wprintf(L"%ls\n", d.Emit().c_str());
            return 4;
        }
        Sleep(250);
    }
}

static int CmdWords(int argc, wchar_t** argv) {
    std::wstring sub = (argc > 2 && argv[2][0] != L'-') ? ToUpper(argv[2]) : L"";

    if (sub == L"PENDING") {
        std::wstring s = L"[";
        int n = 0;
        for (const auto& it : LoadRepeat()) {
            if (it.state != L"proposed") continue;
            if (n++) s += L",";
            s += L"{\"fingerprint\":\"" + it.fp + L"\"";
            s += L",\"count\":" + std::to_wstring(it.count);
            s += L",\"action\":\"" + JsonStr(it.action) + L"\"";
            s += L",\"arg\":\"" + JsonStr(it.arg) + L"\"";
            s += L",\"state\":\"proposed\"}";
        }
        s += L"]";
        wprintf(L"%ls\n", s.c_str());
        return 0;
    }

    if (sub == L"ACCEPT") {
        if (argc < 5) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"words.accept").Bool(L"ok", false);
            d.Int(L"rc", 1).Str(L"error", L"usage: yx words accept <fingerprint> <name>");
            wprintf(L"%ls\n", d.Emit().c_str());
            return 1;
        }
        std::wstring fp = argv[3];
        std::wstring name = argv[4];
        if (!ValidWordName(name)) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"words.accept").Bool(L"ok", false);
            d.Int(L"rc", 1).Str(L"error", L"name must be letters/digits/_/-");
            wprintf(L"%ls\n", d.Emit().c_str());
            return 1;
        }

        auto items = LoadRepeat();
        RepeatItem* hit = nullptr;
        for (auto& it : items) if (it.fp == fp) { hit = &it; break; }
        if (!hit) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"words.accept").Bool(L"ok", false);
            d.Int(L"rc", 1).Str(L"error", L"fingerprint not found");
            wprintf(L"%ls\n", d.Emit().c_str());
            return 1;
        }
        UserWord w;
        w.name = name;
        w.action = hit->action;
        w.arg = hit->arg;
        w.image = hit->image;
        if (!SaveUserWord(w)) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"words.accept").Bool(L"ok", false);
            d.Int(L"rc", 1).Str(L"error", L"word already exists");
            wprintf(L"%ls\n", d.Emit().c_str());
            return 1;
        }
        hit->state = L"active";
        SaveRepeat(items);

        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"words.accept").Bool(L"ok", true);
        d.Str(L"name", name).Str(L"action", w.action).Str(L"arg", w.arg);
        d.Str(L"usage", L"yx run " + name);
        wprintf(L"%ls\n", d.Emit().c_str());
        return 0;
    }

    if (sub == L"REJECT") {
        if (argc < 4) return 1;
        std::wstring fp = argv[3];
        auto items = LoadRepeat();
        bool found = false;
        for (auto& it : items) {
            if (it.fp == fp) { it.state = L"rejected"; found = true; break; }
        }
        if (found) SaveRepeat(items);
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"words.reject").Bool(L"ok", found);
        if (!found) d.Int(L"rc", 1).Str(L"error", L"fingerprint not found");
        wprintf(L"%ls\n", d.Emit().c_str());
        return found ? 0 : 1;
    }

    bool json = GetFlag(argc, argv, L"--json");
    const auto& words = AllWords();
    auto user = LoadUserWords();

    if (json) {
        std::wstring s = L"[";
        bool first = true;
        for (const auto& w : words) {
            if (!first) s += L",";
            first = false;
            s += L"{\"name\":\"" + JsonStr(w.name) + L"\"";
            s += L",\"category\":\"" + JsonStr(w.category) + L"\"";
            s += L",\"desc\":\"" + JsonStr(w.desc) + L"\"";
            s += L",\"kind\":\"";
            s += (w.kind == WordKind::Open) ? L"open" : L"exec";
            s += L"\"}";
        }
        for (const auto& w : user) {
            if (!first) s += L",";
            first = false;
            s += L"{\"name\":\"" + JsonStr(w.name) + L"\"";
            s += L",\"category\":\"学习\",\"desc\":\"学习词\"";
            s += L",\"kind\":\"learned\",\"action\":\"" + JsonStr(w.action) + L"\"}";
        }
        s += L"]";
        wprintf(L"%ls\n", s.c_str());
        return 0;
    }

    wprintf(L"%-22ls %-6ls %ls\n", L"name", L"kind", L"description");
    for (const auto& w : words) {
        wprintf(L"%-22ls %-6ls %ls\n", w.name,
                (w.kind == WordKind::Open) ? L"open" : L"exec", w.desc);
    }
    for (const auto& w : user) {
        wprintf(L"%-22ls %-6ls %ls\n", w.name.c_str(), L"learned", L"学习词");
    }
    wprintf(L"\n共 %zu 个词（静态 %zu + 学习 %zu）。用 yx run <name> 执行。\n",
            words.size() + user.size(), words.size(), user.size());
    return 0;
}

static int CmdWordHelp(int argc, wchar_t** argv) {
    std::wstring name = argc > 2 ? argv[2] : GetOpt(argc, argv, L"--word");
    std::vector<const Word*> cands;
    const Word* w = ResolveWord(name, &cands);
    if (!w) {
        const UserWord* u = FindUserWord(name);
        if (u) {
            JsonDoc d;
            d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"help").Bool(L"ok", true);
            d.Str(L"name", u->name).Str(L"category", L"学习").Str(L"desc", L"学习词");
            d.Str(L"kind", L"learned").Str(L"action", u->action).Str(L"arg", u->arg);
            d.Str(L"usage", std::wstring(L"yx run ") + u->name);
            wprintf(L"%ls\n", d.Emit().c_str());
            return 0;
        }
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"help").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"error", cands.empty() ? (L"unknown word: " + name)
                                                     : (L"ambiguous: " + name));
        if (!cands.empty()) {
            std::wstring s = L"[";
            for (size_t i = 0; i < cands.size(); ++i) {
                if (i) s += L",";
                s += L"\"" + JsonStr(cands[i]->name) + L"\"";
            }
            s += L"]";
            d.Raw(L"candidates", s);
        }
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"help").Bool(L"ok", true);
    d.Str(L"name", w->name).Str(L"category", w->category).Str(L"desc", w->desc);
    d.Str(L"kind", w->kind == WordKind::Open ? L"open" : L"exec");
    d.Str(L"usage", std::wstring(L"yx run ") + w->name);
    wprintf(L"%ls\n", d.Emit().c_str());
    return 0;
}

static int CmdRun(int argc, wchar_t** argv) {
    std::wstring name = argc > 2 ? argv[2] : GetOpt(argc, argv, L"--word");
    std::vector<const Word*> cands;
    const Word* w = ResolveWord(name, &cands);
    if (!w) {
        const UserWord* u = FindUserWord(name);
        if (u) {
            std::vector<std::wstring> store;
            if (u->action == L"read") {
                store = { L"yx", L"read", L"--find", u->arg };
                if (!u->image.empty()) {
                    store.push_back(L"--image");
                    store.push_back(u->image);
                }
            } else if (u->action == L"click-text") {
                store = { L"yx", L"click-text", u->arg };
            } else if (u->action == L"wait") {
                store = { L"yx", L"wait", u->arg };
            } else {
                JsonDoc d;
                d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"run").Bool(L"ok", false);
                d.Int(L"rc", 1).Str(L"error", L"unsupported learned action");
                wprintf(L"%ls\n", d.Emit().c_str());
                return 1;
            }
            std::vector<wchar_t*> av;
            av.reserve(store.size());
            for (auto& s : store) av.push_back(s.data());
            if (u->action == L"read") return CmdRead((int)av.size(), av.data());
            if (u->action == L"click-text") return CmdClickText((int)av.size(), av.data());
            return CmdWait((int)av.size(), av.data());
        }
        JsonDoc d;
        d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"run").Bool(L"ok", false);
        d.Int(L"rc", 1).Str(L"error", cands.empty() ? (L"unknown word: " + name)
                                                     : (L"ambiguous: " + name));
        if (!cands.empty()) {
            std::wstring s = L"[";
            for (size_t i = 0; i < cands.size(); ++i) {
                if (i) s += L",";
                s += L"\"" + JsonStr(cands[i]->name) + L"\"";
            }
            s += L"]";
            d.Raw(L"candidates", s);
        }
        wprintf(L"%ls\n", d.Emit().c_str());
        return 1;
    }

    DWORD t0 = GetTickCount();
    DWORD exitCode = 1;
    bool timedOut = false;
    bool truncated = false;
    bool okProcess = false;
    std::string out;
    std::string err;

    if (w->kind == WordKind::Open) {
        okProcess = RunHandeyeOpen(*w, exitCode, out, err, timedOut, truncated);
    } else {
        std::wstring cmd = L"cmd.exe /d /s /c \"chcp 65001 >nul && ";
        cmd += w->target;
        cmd += L"\"";
        okProcess = RunProcessCapture(cmd, 30000, out, err, exitCode, timedOut, truncated);
    }

    bool ok = okProcess && !timedOut && exitCode == 0;
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"yx").Str(L"op", L"run").Bool(L"ok", ok);
    d.Str(L"word", w->name).Str(L"kind", w->kind == WordKind::Open ? L"open" : L"exec");
    d.Int(L"exit", exitCode).Bool(L"timed_out", timedOut);
    d.Int(L"ms", (long long)(GetTickCount() - t0));
    d.Str(L"stdout", DecodeConsoleBytes(out)).Str(L"stderr", DecodeConsoleBytes(err));
    d.Bool(L"truncated", truncated);
    if (!okProcess) d.Int(L"rc", 3);
    else if (timedOut) d.Int(L"rc", 4);
    wprintf(L"%ls\n", d.Emit().c_str());
    return ok ? 0 : (timedOut ? 4 : 3);
}

static int CmdSchema() {
    std::wstring s = L"{\"v\":\"1\",\"tool\":\"yx\",\"version\":\"";
    s += YX_VERSION;
    s += L"\",\"ops\":[\"status\",\"framing\",\"zoom\",\"open\",\"find\",\"read\",\"click-text\",\"wait\",\"words\",\"words.pending\",\"words.accept\",\"words.reject\",\"help\",\"run\",\"log.verify\"],";
    s += L"\"levels\":[\"L0\",\"L1\",\"L2\"],\"words\":";
    s += std::to_wstring(AllWords().size() + LoadUserWords().size());
    s += L",\"organ\":[";
    bool first = true;
    for (const auto& c : CommandSpecs()) {
        if (!first) s += L",";
        first = false;
        s += L"\"" + std::wstring(c.name) + L"\"";
    }
    s += L"],";
    s += L"\"exit_codes\":{\"0\":\"ok\",\"1\":\"bad argument\",\"3\":\"capture/exec failed\",";
    s += L"\"4\":\"timeout or expired ref\",\"5\":\"ledger verify failed\",\"6\":\"server not running\"}}";
    wprintf(L"%ls\n", s.c_str());
    return 0;
}

// ------------------------------ 入口 ------------------------------

static void PrintUsage() {
    wprintf(L"YX —— 中枢（YX）v%ls\n", YX_VERSION);
    wprintf(L"给 AI 用的观测层：把电脑的真实翻译成 AI 能消化的视图。\n\n");
    wprintf(L"  yx serve                          常驻，监听 %ls\n", PipeName().c_str());
    wprintf(L"  yx status                         问常驻进程要状态（没开就本地回一份）\n");
    wprintf(L"  yx framing [--level L0|L1|L2]     取一帧；默认 L0\n");
    wprintf(L"  yx zoom --ref <ref>               按 ref 回 L2，不重新截屏\n");
    wprintf(L"  yx open <中文名|URL>              打开设置/工具；URL 直接打开\n");
    wprintf(L"  yx find <文字>                    找文字，返回坐标和分数\n");
    wprintf(L"  yx read --find <文字>             只回命中行和后面几行\n");
    wprintf(L"  yx click-text <文字>              找到就点，可加 --expect\n");
    wprintf(L"  yx wait <文字> [--gone]           等文字出现/消失\n");
    wprintf(L"  yx words [--json]                 列出 Windows 命令词\n");
    wprintf(L"  yx words pending                  看重复记忆的待确认词\n");
    wprintf(L"  yx words accept <fp> <name>       确认一个学习词\n");
    wprintf(L"  yx help <词>                      看单个词的说明\n");
    wprintf(L"  yx run <词>                       执行一个词（打开设置页或跑诊断命令）\n");
    wprintf(L"  yx schema                         给 AI 的能力清单（JSON）\n");
    wprintf(L"  yx capture / ocr / uia / click    器官命令（旧 handeye 命令也走这里）\n");
    wprintf(L"  yx log verify [--dir <目录>]      校验账本哈希链和 head 锚\n");
    wprintf(L"  yx help                           这份帮助\n\n");
    wprintf(L"管道协议：u32 长度前缀 + UTF-8 JSON\n");
    wprintf(L"退出码：0 成功 / 1 参数错误 / 3 采集失败 / 4 ref 过期 / 5 校验失败 / 6 服务未就绪\n");
}

int YxMain(int argc, wchar_t** argv) {
    InitConsoleUtf8();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring cmd = argc > 1 ? argv[1] : L"";
    for (wchar_t& c : cmd) c = towlower(c);

    // 只有服务端和 log 命令写账本；普通客户端写 yx-client.log，不污染链。
    if (cmd == L"serve" || cmd == L"log") {
        LogSetApp(L"yx");
        LogEnableChain(true);
    } else {
        LogSetApp(L"yx-client");
    }
    LogInit();
    g_startTick = GetTickCount();

    int rc = 0;
    if (cmd.empty()) {
        PrintUsage();
    } else if (cmd == L"help" || cmd == L"--help" || cmd == L"-h") {
        if (argc > 2) {
            rc = CmdWordHelp(argc, argv);
        } else {
            PrintUsage();
            rc = 0;
        }
    } else if (cmd == L"serve") {
        rc = Serve();
    } else if (cmd == L"status") {
        rc = CmdStatus();
    } else if (cmd == L"framing") {
        rc = CmdFraming(argc, argv);
    } else if (cmd == L"zoom") {
        rc = CmdZoom(argc, argv);
    } else if (cmd == L"open") {
        rc = CmdOpenQuery(argc, argv);
    } else if (cmd == L"find") {
        rc = CmdFind(argc, argv);
    } else if (cmd == L"read") {
        rc = CmdRead(argc, argv);
    } else if (cmd == L"click-text") {
        rc = CmdClickText(argc, argv);
    } else if (cmd == L"wait") {
        rc = CmdWait(argc, argv);
    } else if (cmd == L"words") {
        rc = CmdWords(argc, argv);
    } else if (cmd == L"run") {
        rc = CmdRun(argc, argv);
    } else if (cmd == L"schema") {
        rc = CmdSchema();
    } else if (cmd == L"log") {
        rc = CmdLog(argc, argv);
    } else {
        Say(L"未知命令: %ls", cmd.c_str());
        PrintUsage();
        rc = 1;
    }

    LogShutdown();
    return rc;
}
