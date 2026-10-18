// ============================================================================
//  main.cpp —— handeye 命令行入口（器官层）
//
//  只做两件事：采集、执行。不筛、不排序、不摘要、不验证、不记账。
//  需要视图/账本请用 yx。
//
//  命令：
//    capture / ocr / uia / windows / cursor / foreground / clipboard
//    click / move / type / key / focus / act / say / rec / play / stop
// ============================================================================
#include "common.h"
#include "eye.h"
#include "winenum.h"
#include "input.h"
#include "mouth.h"
#include "log.h"
#include "doctor.h"
#include "schema.h"
#include "uia.h"
#include "clip.h"

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>

#include <winrt/base.h>

// ------------------------------ 参数解析辅助 ------------------------------

// 这些选项后面跟一个值，解析"位置参数"时要连同值一起跳过。
// 清单不在这里维护，统一从 schema 表里查，避免四处各写一份。
static bool IsValueFlag(const std::wstring& s) {
    return OptTakesValue(s);
}

struct ArgList {
    std::vector<std::wstring> raw;
    bool Has(const std::wstring& name) const {
        for (const auto& a : raw) if (a == name) return true;
        return false;
    }
    std::wstring Get(const std::wstring& name, const std::wstring& def = L"") const {
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == name && i + 1 < raw.size()) return raw[i + 1];
        }
        return def;
    }
    int GetInt(const std::wstring& name, int def) const {
        std::wstring v = Get(name, L"");
        if (v.empty()) return def;
        return _wtoi(v.c_str());
    }
    // 位置参数：不以 - 开头、也不是某个选项的值
    std::vector<std::wstring> Positional() const {
        std::vector<std::wstring> out;
        for (size_t i = 0; i < raw.size(); ++i) {
            const std::wstring& a = raw[i];
            if (!a.empty() && a[0] == L'-') {
                if (IsValueFlag(a)) ++i;   // 跳过它的值
                continue;
            }
            out.push_back(a);
        }
        return out;
    }
};

static bool ParseRect(const std::wstring& s, RectI& out) {
    auto p = Split(s, L',');
    if (p.size() != 4) return false;
    out.x = _wtoi(p[0].c_str());
    out.y = _wtoi(p[1].c_str());
    out.w = _wtoi(p[2].c_str());
    out.h = _wtoi(p[3].c_str());
    return out.w > 0 && out.h > 0;
}

static std::wstring RedactArgv(const std::vector<std::wstring>& raw) {
    std::wstring s;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (i) s += L' ';
        s += raw[i];
    }
    return LogRedactText(s);
}

static int Dispatch(const ArgList& args, const std::wstring& cmd);

// ------------------------------ raw collection ------------------------------

// 把命令行开关翻译成一次识别请求。
// 默认 auto：中文走 1x，拉丁/数字行再跑一路英文；--fast 只跑中文 1x。
static OcrRequest BuildOcrRequest(const ArgList& args, const RectI& region) {
    OcrRequest req;
    req.region = region;

    std::wstring sv = ToUpper(args.Get(L"--scale"));
    if (sv == L"1") req.scale = 1;
    else if (sv == L"2") req.scale = 2;
    else if (sv == L"3") req.scale = 3;
    else req.scale = 0;

    if (args.Has(L"--fast")) req.scale = 1;

    req.lang = args.Get(L"--lang", L"auto");
    if (req.lang.empty()) req.lang = L"auto";
    req.padPx = args.GetInt(L"--pad", 8);
    req.merge = !args.Has(L"--fast") && req.scale == 0;
    req.secondLook = args.Has(L"--best");
    return req;
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
        s += L",\"words\":" + std::to_wstring(l.words);
        wchar_t q[32] = {};
        swprintf_s(q, L"%.3f", l.quality);
        s += L",\"q\":" + std::wstring(q);
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
        s += n.enabled ? L",\"enabled\":true" : L",\"enabled\":false";
        s += n.offscreen ? L",\"offscreen\":true" : L",\"offscreen\":false";
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

static int FailJson(const wchar_t* op, const wchar_t* error, int rc) {
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"handeye").Str(L"op", op)
     .Bool(L"ok", false).Int(L"rc", rc).Str(L"error", error);
    wprintf(L"%ls\n", d.Emit().c_str());
    return rc;
}

static int CmdCapture(const ArgList& args) {
    RectI region{};
    std::wstring rs = args.Get(L"--region");
    if (!rs.empty() && !ParseRect(rs, region)) return FailJson(L"capture", L"bad --region", 1);

    std::wstring save = args.Get(L"--save");
    DWORD t0 = GetTickCount();
    bool ok = true;
    if (!save.empty()) ok = EyeSavePng(save, region);

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"handeye").Str(L"op", L"capture").Bool(L"ok", ok);
    d.Raw(L"screen", RectJson(EyeScreenRect()));
    d.Raw(L"region", RectJson(region));
    if (!save.empty()) d.Str(L"path", save);
    d.Bool(L"saved", !save.empty() && ok);
    d.Int(L"ms", (long long)(GetTickCount() - t0));
    wprintf(L"%ls\n", d.Emit().c_str());
    return ok ? 0 : 3;
}

static int CmdOcr(const ArgList& args) {
    RectI region{};
    std::wstring rs = args.Get(L"--region");
    if (!rs.empty() && !ParseRect(rs, region)) return FailJson(L"ocr", L"bad --region", 1);

    DWORD t0 = GetTickCount();
    OcrResult r;
    std::wstring image = args.Get(L"--image");
    if (!image.empty()) r = EyeRecognizeImageFile(image);
    else {
        OcrRequest req = BuildOcrRequest(args, region);
        r = EyeRecognize(req);
    }

    if (!r.ok) return FailJson(L"ocr", r.error.empty() ? L"ocr failed" : r.error.c_str(), 3);

    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"handeye").Str(L"op", L"ocr").Bool(L"ok", true);
    d.Raw(L"region", RectJson(r.region));
    d.Int(L"scale", r.scale);
    d.Int(L"raw", (long long)r.lines.size());
    d.Raw(L"lines", LinesJson(r.lines));
    d.Int(L"ms", (long long)(GetTickCount() - t0));
    wprintf(L"%ls\n", d.Emit().c_str());
    return 0;
}

static int CmdUia(const ArgList& args) {
    if (!UiInit()) return FailJson(L"uia", L"ui automation unavailable", 3);
    DWORD t0 = GetTickCount();
    auto nodes = UiTree(nullptr,
                        args.GetInt(L"--depth", 8),
                        args.GetInt(L"--max", 800),
                        args.GetInt(L"--timeout", 400));
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"handeye").Str(L"op", L"uia").Bool(L"ok", true);
    d.Str(L"window", GetForegroundTitle());
    d.Int(L"nodes", (long long)nodes.size());
    d.Raw(L"items", UiJson(nodes));
    d.Int(L"ms", (long long)(GetTickCount() - t0));
    wprintf(L"%ls\n", d.Emit().c_str());
    return 0;
}

static int CmdWindows() {
    DWORD t0 = GetTickCount();
    auto wins = EnumTopWindows();
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"handeye").Str(L"op", L"windows").Bool(L"ok", true);
    d.Int(L"count", (long long)wins.size());
    d.Raw(L"windows", WindowsJson(wins));
    d.Int(L"ms", (long long)(GetTickCount() - t0));
    wprintf(L"%ls\n", d.Emit().c_str());
    return 0;
}

static int CmdCursor() {
    POINT p = GetCursorPosNow();
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"handeye").Str(L"op", L"cursor").Bool(L"ok", true);
    d.Int(L"x", p.x).Int(L"y", p.y);
    wprintf(L"%ls\n", d.Emit().c_str());
    return 0;
}

static int CmdForeground() {
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"handeye").Str(L"op", L"foreground").Bool(L"ok", true);
    d.Str(L"title", GetForegroundTitle());
    wprintf(L"%ls\n", d.Emit().c_str());
    return 0;
}

static int CmdClipboard() {
    std::wstring text;
    bool ok = ClipGetText(text);
    JsonDoc d;
    d.Str(L"v", L"1").Str(L"tool", L"handeye").Str(L"op", L"clipboard").Bool(L"ok", ok);
    d.Str(L"text", text);
    wprintf(L"%ls\n", d.Emit().c_str());
    return ok ? 0 : 3;
}

// ------------------------------ 能力自述 ------------------------------

// 能力清单和帮助都从 schema.cpp 的同一张表生成。
// 以前 IsValueFlag / PrintUsage / PrintSchema 各写一份，对不上是必然的。
static void PrintSchema() { PrintSchemaFromTable(); }

// ------------------------------ 帮助 ------------------------------

static void PrintUsage() { PrintUsageFromTable(); }

int wmain(int argc, wchar_t** argv) {
    InitConsoleUtf8();

    // DPI：用 PerMonitorV2，多显示器下坐标才正确
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // OCR 与语音合成都是 WinRT，必须先初始化 COM 公寓。
    // 用 MTA：OCR 的异步调用在 MTA 上不会像 STA 那样有死锁风险。
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        // 已初始化过就忽略
    }

    ArgList args;
    for (int i = 1; i < argc; ++i) args.raw.push_back(argv[i]);

    // 先起日志，再处理任何东西 —— 参数错误也要留痕
    LogInit();
    {
        bool hasLevel = args.Has(L"--log-level");
        bool hasText = args.Has(L"--log-text");
        bool hasDir = args.Has(L"--log-dir");
        int textMode = g_log.textMode;
        std::wstring tv = ToUpper(args.Get(L"--log-text"));
        if (tv == L"MASK") textMode = LOG_TEXT_MASK;
        else if (tv == L"OFF") textMode = LOG_TEXT_OFF;
        else if (tv == L"FULL") textMode = LOG_TEXT_FULL;
        LogConfigure(hasDir, args.Get(L"--log-dir"),
                     hasLevel, LogLevelFromName(args.Get(L"--log-level")),
                     hasText, textMode,
                     args.Has(L"--no-log"), false);
        if (args.Has(L"--log-echo")) g_log.echoToStderr = true;
    }

    g_opt.json = args.Has(L"--json");
    g_opt.dryRun = args.Has(L"--dry-run");
    g_opt.maxDurationSec = args.GetInt(L"--max-duration", 0);
    g_opt.announce = args.Has(L"--announce");
    g_opt.instantMove = args.Has(L"--instant");
    EyeSetTimeout(args.GetInt(L"--ocr-timeout", 20000));

    // 从进程启动就开始计时，--max-duration 才能覆盖"等屏幕"这种没有鼠标的阶段
    AbortInit();

    if (args.raw.empty()) {
        PrintUsage();
        LogShutdown();
        return 0;
    }

    std::wstring cmd = args.raw[0];
    for (wchar_t& c : cmd) c = towlower(c);

    // 会话开始：把完整命令行记下来，事后能复原"AI 到底调了什么"
    {
        std::wstring argvJoin = RedactArgv(args.raw);
        if (argvJoin.size() > 400) argvJoin = argvJoin.substr(0, 400) + L"…";
        wchar_t cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        LogI(L"会话", 8000, L"会话", L"会话开始", L"argv=\"%ls\" cwd=%ls",
             argvJoin.c_str(), cwd);
    }

    int rc = Dispatch(args, cmd);

    // 只会 Say() 的命令（act / click / type / say / stop ...）在 --json 模式下
    // 本来什么都不输出，警告也一起丢了。这里补一个收尾对象，把 ok/rc/warnings 交代清楚。
    {
        bool manualJson = (cmd == L"schema" || cmd == L"doctor" ||
                           cmd == L"help" || cmd == L"--help" || cmd == L"-h");
        if (g_opt.json && !manualJson && !JsonWasEmitted()) {
            JsonDoc d;
            d.Bool(L"ok", rc == 0);
            if (rc != 0) d.Int(L"rc", rc);
            wprintf(L"%ls\n", d.Emit().c_str());
        }
    }

    {
        DWORD ms = GetTickCount() - LogStartTick();
        if (rc == 0)
            LogI(L"会话", 8001, L"会话", L"会话结束", L"rc=0 ms=%lu", (unsigned long)ms);
        else
            // A non-zero exit code is a normal outcome (not found, blocked,
            // verification failed), not a warning. Logging it as a warning
            // drowned the doctor output in noise.
            LogI(L"会话", 8001, L"会话", L"会话结束", L"rc=%d ms=%lu", rc, (unsigned long)ms);
    }
    LogShutdown();
    return rc;
}

static int Dispatch(const ArgList& args, const std::wstring& cmd) {
    if (cmd == L"help" || cmd == L"--help" || cmd == L"-h") { PrintUsage(); return 0; }
    if (cmd == L"schema") { PrintSchema(); return 0; }
    if (cmd == L"doctor") return CmdDoctor(args.raw);

    // ---------- 采集（raw） ----------
    if (cmd == L"capture")    return CmdCapture(args);
    if (cmd == L"ocr")        return CmdOcr(args);
    if (cmd == L"uia")        return CmdUia(args);
    if (cmd == L"windows")    return CmdWindows();
    if (cmd == L"cursor")     return CmdCursor();
    if (cmd == L"foreground") return CmdForeground();
    if (cmd == L"clipboard")  return CmdClipboard();

    auto pos = args.Positional();   // pos[0] 是命令本身

    // ---------- 手 ----------
    if (cmd == L"act" || cmd == L"script") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少脚本路径\n"); return 1; }
        InputInit();
        int rc = RunScriptFile(pos[1]);
        InputShutdown();
        return rc;
    }
    if (cmd == L"click") {
        if (pos.size() < 3) { wprintf(L"ERR 用法: click <x> <y> [--right]\n"); return 1; }
        InputInit();
        ClickAt(_wtoi(pos[1].c_str()), _wtoi(pos[2].c_str()), args.Has(L"--right"));
        InputShutdown();
        return 0;
    }
    if (cmd == L"move") {
        if (pos.size() < 3) { wprintf(L"ERR 用法: move <x> <y>\n"); return 1; }
        InputInit();
        MoveTo(_wtoi(pos[1].c_str()), _wtoi(pos[2].c_str()));
        InputShutdown();
        return 0;
    }
    if (cmd == L"type") {
        std::wstring text;
        for (size_t i = 1; i < pos.size(); ++i) {
            if (!text.empty()) text += L" ";
            text += pos[i];
        }
        if (text.empty()) { wprintf(L"ERR 缺少要输入的文字\n"); return 1; }
        InputInit();
        bool ok = TypeText(text);
        InputShutdown();
        return ok ? 0 : 4;
    }
    if (cmd == L"key") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少键名\n"); return 1; }
        InputInit();
        bool ok = false;
        if (pos[1].find(L'+') != std::wstring::npos) {
            ok = PressKeyCombo(pos[1]);
        } else {
            int vk = 0;
            bool ext = false;
            if (ResolveKeyName(pos[1], vk, ext)) { PressKey(vk, ext); ok = true; }
            else if (pos[1].size() == 1) { ok = TypeText(pos[1]); }
        }
        InputShutdown();
        if (!ok) { wprintf(L"ERR 无法识别的键: %ls\n", pos[1].c_str()); return 1; }
        return 0;
    }
    if (cmd == L"focus") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少窗口标题片段\n"); return 1; }
        InputInit();
        bool ok = FocusWindow(pos[1]);
        InputShutdown();
        if (!ok) { wprintf(L"ERR 找不到窗口: %ls\n", pos[1].c_str()); return 1; }
        return 0;
    }
    if (cmd == L"rec" || cmd == L"record") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少文件路径\n"); return 1; }
        InputInit();
        int rc = DoRecord(pos[1], args.Has(L"--auto"));
        InputShutdown();
        return rc;
    }
    if (cmd == L"play") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少文件路径\n"); return 1; }
        InputInit();
        int rc = DoPlay(pos[1], args.GetInt(L"--loop", 1));
        InputShutdown();
        return rc;
    }
    if (cmd == L"dump") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少文件路径\n"); return 1; }
        return DoDump(pos[1]);
    }
    if (cmd == L"stop") return DoStop();

    // ---------- 嘴 ----------
    if (cmd == L"voices") {
        auto list = MouthListVoices();
        if (g_opt.json) {
            std::wstring listJson = L"[";
            for (size_t i = 0; i < list.size(); ++i) {
                if (i) listJson += L",";
                listJson += L"{\"name\":\"" + JsonStr(list[i].name) + L"\"";
                listJson += L",\"lang\":\"" + JsonStr(list[i].language) + L"\"}";
            }
            listJson += L"]";
            JsonDoc d;
            d.Bool(L"ok", true).Raw(L"voices", listJson);
            wprintf(L"%ls\n", d.Emit().c_str());
        } else {
            for (const auto& v : list)
                wprintf(L"%ls\t%ls\n", v.name.c_str(), v.language.c_str());
        }
        return list.empty() ? 3 : 0;
    }
    if (cmd == L"say" || cmd == L"speak") {
        std::wstring text;
        for (size_t i = 1; i < pos.size(); ++i) {
            if (!text.empty()) text += L" ";
            text += pos[i];
        }
        std::wstring textFile = args.Get(L"--text-file");
        if (!textFile.empty()) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, textFile.c_str(), L"rb") == 0 && f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                std::vector<char> buf(size_t(sz > 0 ? sz : 0) + 1, 0);
                size_t got = sz > 0 ? fread(buf.data(), 1, size_t(sz), f) : 0;
                fclose(f);
                text = FromUtf8(std::string(buf.data(), got));
            } else {
                wprintf(L"ERR 读不到文本文件: %ls\n", textFile.c_str());
                return 2;
            }
        }
        int rc = MouthSpeak(text,
                            args.Get(L"--voice"),
                            args.GetInt(L"--rate", 0),
                            args.GetInt(L"--volume", 100),
                            args.Get(L"--wav"),
                            args.Has(L"--async"));
        return rc;
    }

    wprintf(L"未知命令: %ls\n\n", cmd.c_str());
    PrintUsage();
    return 1;
}
