# YX

给 AI 用的一双眼睛。

你让 AI 看屏幕，它不用先截一张几 MB 的图再自己猜。YX 把当前屏幕压成一句话：

```
[L0] 窗口「另存为」| 可操作 3 项 | 无错误
```

需要细节时再 `yx zoom`，拿回原始 OCR 行和控件树。

## 现在能做什么

- `yx framing --level L0|L1|L2`：取一帧，按级别给视图
- `yx zoom --ref <ref>`：按引用回 L2，不重新截屏
- `yx status`：服务在不在、版本、管道、数据目录
- `yx words` / `yx words --json`：列出 30+ 个常用 Windows 命令词
- `yx help <词>`：看单个词做什么
- `yx run <词>`：执行；打开设置页/工具，或跑一条只读诊断命令并返回输出
- `yx schema`：给 AI 的 JSON 能力清单
- `yx log verify`：校验账本哈希链和 `head` 锚，改一行/删末行会报错
- 器官命令也在同一个 exe 里：截屏、OCR、UIA、窗口、光标、剪贴板；点击、输入、按键、脚本、语音
- 任何 AI 都能调：命令行或命名管道 `\\.\pipe\yx-<用户名>`

## 给 AI 的词

```powershell
.\yx.exe run devmgmt     # 打开设备管理器
.\yx.exe run settings    # 打开设置
.\yx.exe run ipconfig    # 跑 ipconfig /all，返回 stdout
.\yx.exe run whoami      # 当前用户和权限
```

词表是人工审核过的，AI 只发词名，不拼命令、不做参数替换。
危险命令（格式化、关机、分区、删注册表）故意不放。

## 跑起来

需要 Windows 10/11，装好 Windows 的 OCR 语言包（中文/英文）。

```powershell
.\yx.exe serve
```

另开一个窗口：

```powershell
.\yx.exe framing --level L0
```

或者双击 `demo.bat`：它会启动服务并打印一条 L0。

## 自己编译

需要 Visual Studio 2022 的“使用 C++ 的桌面开发”。

```powershell
.\build.ps1
```

产物只有一个 `yx.exe`。旧命令可以用 `handeye.bat` 兼容。

## 给开发者

管道协议、请求/响应示例和错误码在 [docs/protocol.md](docs/protocol.md)。

任何语言都可以连：发一个 `u32` 长度加 UTF-8 JSON，读回一个同样格式的响应。
不需要 SDK，也不需要 DSH。

## 它不是什么

- 不是截图工具：它给的是投影，不是原图。
- 不是一个模型：同输入同输出，不联网，不调用大模型。
- 不是大厂项目：没有 CI、没有企业级部署、没有“生产可用”的承诺。
  它先给你自己的电脑用。

## 现在不做

文本压缩、AI 自动生成词表、记忆、托盘、本地 HTTP、GPU OCR、账本的 HMAC/外部锚。
这些以后再说，v0.3 先把“一个 exe、能看见、能点、能查、能用一句话开 Windows 功能”做稳。

## 一个程序，两层

- 器官层：采集和执行，不整理、不判断、不记账。
- 中枢层：把一次采集投影成 L0/L1/L2，管最近几帧，按 `ref` 回看，记账，执行词。
- 兼容：旧脚本里的 `handeye xxx` 用 `handeye.bat xxx`，参数原样转发。

AI 是皮层：它负责理解和决定。YX 不替它理解。

## 限制

- L0 是启发式，不是“保证看懂”；复杂界面会不准。
- UI Automation 在不同软件里质量差别很大。
- 报警通道（对话框、错误词、新窗口）优先保留，所以有时候 L0 会超过一行。
- 账本链只能发现误改和局部改；能改整个文件的人可以重算整条链。`head` 锚能发现删末行，
  但它本身也能被改。完整边界见 [docs/limitations.md](docs/limitations.md)。

## License

MIT。见 [LICENSE](LICENSE)。
