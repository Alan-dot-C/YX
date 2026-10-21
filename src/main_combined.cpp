// ============================================================================
//  main_combined.cpp —— 单个 yx.exe 的入口
//
//  器官命令走 HandeyeMain，其余走 YxMain。
//  handeye.bat 只是把旧命令名转发到这里，兼容旧脚本。
// ============================================================================
#include <windows.h>
#include <string>
#include <cwctype>

int HandeyeMain(int argc, wchar_t** argv);
int YxMain(int argc, wchar_t** argv);

static bool IsOrganCommand(const std::wstring& cmd) {
    static const wchar_t* kOrgan[] = {
        L"capture", L"ocr", L"uia", L"windows", L"cursor", L"foreground",
        L"clipboard", L"open", L"click", L"move", L"type", L"key", L"focus",
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
    if (IsOrganCommand(cmd)) return HandeyeMain(argc, argv);
    return YxMain(argc, argv);
}
