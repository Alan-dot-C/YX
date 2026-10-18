// ============================================================================
//  schema.cpp —— handeye 1.2 的命令与选项表
//
//  handeye 是器官层：只负责采集和执行。它不做筛子、排序、摘要、验证或记账。
//  需要 L0/L1 投影、压缩或账本时，用 yx。
// ============================================================================
#include "schema.h"
#include "common.h"

#include <cstdio>

// ------------------------------ 选项表 ------------------------------

static const OptSpec kGlobalOpts[] = {
    { L"--json",         false, L"输出单个 JSON 对象" },
    { L"--dry-run",      false, L"只说不做" },
    { L"--max-duration", true,  L"秒；超过就中止" },
    { L"--announce",     false, L"动作前先语音播报" },
    { L"--instant",      false, L"鼠标直接跳到位，不做缓动" },
    { L"--log-dir",      true,  L"日志目录" },
    { L"--log-level",    true,  L"日志级别：严重/错误/警告/信息/详细" },
    { L"--log-text",     true,  L"文本落盘策略：full/mask/off" },
    { L"--no-log",       false, L"本次不写日志" },
    { L"--log-echo",     false, L"同时打到 stderr" },
    { L"--ocr-timeout",  true,  L"单次 OCR 超时毫秒（默认 20000）" },
    { nullptr, false, nullptr }
};

static const OptSpec kCaptureOpts[] = {
    { L"--region", true,  L"只截这一块区域 x,y,w,h（空 = 全屏）" },
    { L"--save",   true,  L"保存为 PNG 的路径" },
    { nullptr, false, nullptr }
};

static const OptSpec kOcrOpts[] = {
    { L"--region", true,  L"只识别这一块区域 x,y,w,h" },
    { L"--image",  true,  L"识别一张图片，而不是当前屏幕（固定样本用）" },
    { L"--scale",  true,  L"送 OCR 前的放大倍数：auto(默认) / 1 / 2 / 3" },
    { L"--lang",   true,  L"识别引擎：auto(默认，按行选) / zh / en" },
    { L"--pad",    true,  L"裁切时四周留白像素（默认 8）" },
    { L"--fast",   false, L"只跑中文 1x，最快" },
    { L"--best",   false, L"双引擎合并 + 可疑行二次识别，最准" },
    { nullptr, false, nullptr }
};

static const OptSpec kUiaOpts[] = {
    { L"--depth",   true, L"最大深度（默认 8）" },
    { L"--max",     true, L"最多节点数（默认 800）" },
    { L"--timeout", true, L"UIA 查询预算毫秒（默认 400）" },
    { nullptr, false, nullptr }
};

static const OptSpec kClickOpts[] = {
    { L"--right", false, L"右键点击" },
    { nullptr, false, nullptr }
};

static const OptSpec kSayOpts[] = {
    { L"--voice",     true,  L"语音名（见 voices）" },
    { L"--rate",      true,  L"语速 -10..10" },
    { L"--volume",    true,  L"音量 0..100" },
    { L"--wav",       true,  L"导出为 wav 而不是播放" },
    { L"--text-file", true,  L"从文件读要播报的文字" },
    { L"--async",     false, L"不等待播完" },
    { nullptr, false, nullptr }
};

static const OptSpec kRecOpts[] = {
    { L"--auto", false, L"启动即开始录" },
    { nullptr, false, nullptr }
};

static const OptSpec kPlayOpts[] = {
    { L"--loop", true, L"循环次数，0 = 无限" },
    { nullptr, false, nullptr }
};

static const OptSpec kDoctorOpts[] = {
    { L"--since", true, L"时间窗口，如 30m / 2h / 1d" },
    { nullptr, false, nullptr }
};

// ------------------------------ 命令表 ------------------------------

static const CmdSpec kCmds[] = {
    // 采集
    { L"capture",   L"capture [选项]",          L"截屏；可选保存 PNG", kCaptureOpts },
    { L"ocr",       L"ocr [选项]",              L"原始 OCR 行（文字、坐标、置信度）", kOcrOpts },
    { L"uia",       L"uia [选项]",              L"当前窗口的原始 UI Automation 控件树", kUiaOpts },
    { L"windows",   L"windows",                 L"可见顶层窗口列表（Z 序）", nullptr },
    { L"cursor",    L"cursor",                  L"当前鼠标坐标", nullptr },
    { L"foreground",L"foreground",              L"当前前台窗口标题", nullptr },
    { L"clipboard", L"clipboard",               L"读取剪贴板文本", nullptr },

    // 执行
    { L"click",     L"click <x> <y> [选项]",    L"点击坐标", kClickOpts },
    { L"move",      L"move <x> <y>",            L"移动鼠标", nullptr },
    { L"type",      L"type <文字>",             L"输入文字（支持中文与任意 Unicode）", nullptr },
    { L"key",       L"key <键名或组合键>",      L"按键，如 ENTER / CTRL+S / 0x41", nullptr },
    { L"focus",     L"focus <标题片段>",        L"把窗口切到前台", nullptr },
    { L"act",       L"act <脚本.txt>",          L"执行底层动作脚本", nullptr },
    { L"say",       L"say <文字> [选项]",       L"语音播报", kSayOpts },
    { L"voices",    L"voices",                  L"列出可用语音", nullptr },
    { L"rec",       L"rec <文件.rec> [选项]",   L"录制", kRecOpts },
    { L"play",      L"play <文件.rec> [选项]",  L"回放", kPlayOpts },
    { L"dump",      L"dump <文件.rec>",         L"打印录制内容", nullptr },
    { L"stop",      L"stop",                    L"停止正在运行的动作", nullptr },

    // 诊断
    { L"doctor",    L"doctor [选项]",           L"检查 OCR / UIA / 日志环境", kDoctorOpts },
    { L"schema",    L"schema",                  L"输出这份能力清单", nullptr },
    { L"help",      L"help",                    L"输出帮助", nullptr },
};

const std::vector<CmdSpec>& CommandSpecs() {
    static const std::vector<CmdSpec> v(std::begin(kCmds), std::end(kCmds));
    return v;
}

bool OptTakesValue(const std::wstring& name) {
    for (const auto& o : kGlobalOpts) {
        if (o.name && name == o.name) return o.takesValue;
    }
    for (const auto& c : CommandSpecs()) {
        if (!c.opts) continue;
        for (const OptSpec* o = c.opts; o->name; ++o) {
            if (name == o->name) return o->takesValue;
        }
    }
    return false;
}

// ------------------------------ 帮助 ------------------------------

void PrintUsageFromTable() {
    wprintf(L"handeye 1.2 —— 器官层：采集 + 执行\n");
    wprintf(L"它不做筛子、排序、摘要、验证或记账；需要视图和账本请用 yx。\n\n");

    for (const auto& c : CommandSpecs()) {
        wprintf(L"  handeye %-30ls %ls\n", c.usage, c.desc);
    }

    wprintf(L"\n采集命令默认输出 JSON；动作命令可用 --json 包一层结果。\n");
    wprintf(L"退出码: 0 成功 / 1 参数错误 / 2 文件错误 / 3 执行失败 / 4 超时或被停止\n");

    wprintf(L"\n全局选项:\n");
    for (const OptSpec* o = kGlobalOpts; o->name; ++o) {
        wprintf(L"  %-20ls %ls\n", o->name, o->help);
    }

    wprintf(L"\nocr 的选项:\n");
    for (const OptSpec* o = kOcrOpts; o->name; ++o) {
        wprintf(L"  %-20ls %ls\n", o->name, o->help);
    }

    wprintf(L"\nuia 的选项:\n");
    for (const OptSpec* o = kUiaOpts; o->name; ++o) {
        wprintf(L"  %-20ls %ls\n", o->name, o->help);
    }
}

// ------------------------------ 能力清单 ------------------------------

static void PrintJsonStringArray(const OptSpec* opts) {
    wprintf(L"[");
    bool first = true;
    for (const OptSpec* o = opts; o->name; ++o) {
        if (!first) wprintf(L",");
        first = false;
        wprintf(L"\"%ls\"", o->name);
    }
    wprintf(L"]");
}

void PrintSchemaFromTable() {
    wprintf(L"{\n");
    wprintf(L"  \"tool\": \"handeye\",\n");
    wprintf(L"  \"version\": \"1.2.0\",\n");
    wprintf(L"  \"layer\": \"organ\",\n");
    wprintf(L"  \"purpose\": \"采集屏幕/控件/窗口，执行鼠标键盘；不处理、不验证、不记账。\",\n");

    wprintf(L"  \"commands\": [\n");
    const auto& cmds = CommandSpecs();
    for (size_t i = 0; i < cmds.size(); ++i) {
        const auto& c = cmds[i];
        wprintf(L"    {\"name\":\"%ls\",\"usage\":\"%ls\",\"desc\":\"%ls\"",
                c.name, c.usage, c.desc);
        if (c.opts) {
            wprintf(L",\"options\":");
            PrintJsonStringArray(c.opts);
        }
        wprintf(L"}%ls\n", (i + 1 < cmds.size()) ? L"," : L"");
    }
    wprintf(L"  ],\n");

    wprintf(L"  \"globalOptions\": ");
    PrintJsonStringArray(kGlobalOpts);
    wprintf(L",\n");

    wprintf(L"  \"exitCodes\": {\"0\":\"成功\",\"1\":\"参数错误\",\"2\":\"文件错误\","
            L"\"3\":\"执行失败\",\"4\":\"超时或被停止\"},\n");

    wprintf(L"  \"notes\": [\n");
    wprintf(L"    \"采集结果给 yx，由 yx 做 L0/L1/L2 投影和报警抽取\",\n");
    wprintf(L"    \"动作命令只保证输入已发送；是否生效由调用方或 yx 验证\",\n");
    wprintf(L"    \"ocr --image 用于固定样本回归，不依赖当前屏幕\",\n");
    wprintf(L"    \"handeye 不写审计账本；账本在 yx.log\"\n");
    wprintf(L"  ]\n");
    wprintf(L"}\n");
}
