// ============================================================================
//  main_combined.cpp —— 单个 yx.exe 的入口
//
//  器官命令走 HandeyeMain，其余走 YxMain。
//  handeye.bat 只是把旧命令名转发到这里，兼容旧脚本。
// ============================================================================
#include <windows.h>
#include <string>
#include <vector>
#include <cwctype>
#include <cstdlib>

int HandeyeMain(int argc, wchar_t** argv);
int YxMain(int argc, wchar_t** argv);

static bool IsNumber(const wchar_t* s) {
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    wcstol(s, &end, 10);
    return end && *end == 0;
}

static bool IsOrganCommand(const std::wstring& cmd) {
    static const wchar_t* kOrgan[] = {
        L"capture", L"ocr", L"uia", L"windows", L"cursor", L"foreground",
        L"clipboard", L"click", L"move", L"type", L"key", L"focus",
        L"act", L"say", L"voices", L"rec", L"play", L"dump", L"stop",
        L"doctor",
    };
    for (const wchar_t* c : kOrgan) {
        if (_wcsicmp(cmd.c_str(), c) == 0) return true;
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return YxMain(argc, argv);

    std::wstring cmd = argv[1];
    for (wchar_t& c : cmd) c = towlower(c);

    // 输入模糊化：click 两个数字 = 坐标点击；其余当成 click-text。
    if (cmd == L"click" && !(argc >= 4 && IsNumber(argv[2]) && IsNumber(argv[3]))) {
        std::vector<wchar_t*> av;
        av.push_back(argv[0]);
        av.push_back(const_cast<wchar_t*>(L"click-text"));
        for (int i = 2; i < argc; ++i) av.push_back(argv[i]);
        av.push_back(nullptr);
        return YxMain((int)av.size() - 1, av.data());
    }

    if (IsOrganCommand(cmd)) return HandeyeMain(argc, argv);
    return YxMain(argc, argv);
}
