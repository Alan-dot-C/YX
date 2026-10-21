// ============================================================================
//  words.cpp —— 给 AI 用的 Windows 命令词表（精简版）
//
//  只保留最常用的 32 个入口。人工审核，AI 只发词名，不拼命令。
//  危险命令（格式化、关机、分区、删注册表等）不放。
// ============================================================================
#include "words.h"

#include <cwchar>

static const Word kWords[] = {
    // 设置
    { L"settings",  L"设置", L"Windows 设置首页",       WordKind::Open, L"ms-settings:", nullptr, L"设置|设置页" },
    { L"display",   L"设置", L"显示设置",               WordKind::Open, L"ms-settings:display", nullptr },
    { L"sound",     L"设置", L"声音设置",               WordKind::Open, L"ms-settings:sound", nullptr },
    { L"network",   L"设置", L"网络状态",               WordKind::Open, L"ms-settings:network-status", nullptr },
    { L"bluetooth", L"设置", L"蓝牙",                   WordKind::Open, L"ms-settings:bluetooth", nullptr },
    { L"update",    L"设置", L"Windows 更新",           WordKind::Open, L"ms-settings:windowsupdate", nullptr },
    { L"apps",      L"设置", L"已安装应用",             WordKind::Open, L"ms-settings:appsfeatures", nullptr },
    { L"power",     L"设置", L"电源和睡眠",             WordKind::Open, L"ms-settings:powersleep", nullptr },
    { L"about",     L"设置", L"关于本机",               WordKind::Open, L"ms-settings:about", nullptr },

    // 工具
    { L"taskmgr",   L"工具", L"任务管理器",             WordKind::Open, L"taskmgr.exe", nullptr },
    { L"devmgmt",   L"工具", L"设备管理器",             WordKind::Open, L"devmgmt.msc", nullptr },
    { L"services",  L"工具", L"服务",                   WordKind::Open, L"services.msc", nullptr },
    { L"diskmgmt",  L"工具", L"磁盘管理",               WordKind::Open, L"diskmgmt.msc", nullptr },
    { L"eventvwr",  L"工具", L"事件查看器",             WordKind::Open, L"eventvwr.msc", nullptr },
    { L"control",   L"工具", L"控制面板",               WordKind::Open, L"control.exe", nullptr },
    { L"msconfig",  L"工具", L"系统配置",               WordKind::Open, L"msconfig.exe", nullptr },
    { L"resmon",    L"工具", L"资源监视器",             WordKind::Open, L"resmon.exe", nullptr },
    { L"msinfo32",  L"工具", L"系统信息",               WordKind::Open, L"msinfo32.exe", nullptr },

    // 程序
    { L"notepad",   L"程序", L"记事本",                 WordKind::Open, L"notepad.exe", nullptr },
    { L"calc",      L"程序", L"计算器",                 WordKind::Open, L"calc.exe", nullptr },
    { L"explorer",  L"程序", L"文件资源管理器",         WordKind::Open, L"explorer.exe", nullptr },
    { L"cmd",       L"程序", L"命令提示符",             WordKind::Open, L"cmd.exe", nullptr },
    { L"powershell",L"程序", L"PowerShell",             WordKind::Open, L"powershell.exe", nullptr },

    // 命令
    { L"ipconfig",  L"命令", L"ipconfig /all",          WordKind::Exec, L"ipconfig /all", nullptr },
    { L"ping",      L"命令", L"ping 本机 127.0.0.1",    WordKind::Exec, L"ping 127.0.0.1", nullptr },
    { L"netstat",   L"命令", L"netstat -ano",           WordKind::Exec, L"netstat -ano", nullptr },
    { L"tasklist",  L"命令", L"当前进程列表",           WordKind::Exec, L"tasklist", nullptr },
    { L"systeminfo",L"命令", L"系统信息（较慢）",       WordKind::Exec, L"systeminfo", nullptr },
    { L"whoami",    L"命令", L"当前用户和权限",         WordKind::Exec, L"whoami /all", nullptr },
    { L"hostname",  L"命令", L"计算机名",               WordKind::Exec, L"hostname", nullptr },
    { L"getmac",    L"命令", L"网卡 MAC 地址",          WordKind::Exec, L"getmac /v", nullptr },
    { L"ver",       L"命令", L"Windows 版本",           WordKind::Exec, L"ver", nullptr },
};

const std::vector<Word>& AllWords() {
    static const std::vector<Word> v(std::begin(kWords), std::end(kWords));
    return v;
}

const Word* FindWord(const std::wstring& name) {
    for (const auto& w : AllWords()) {
        if (_wcsicmp(w.name, name.c_str()) == 0) return &w;
    }
    return nullptr;
}
