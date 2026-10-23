# YX

给 AI 用的一双眼睛。一个 `yx.exe`，零第三方依赖，Windows 10/11 本地运行。

> **如果你是 AI，先读 [docs/for-ai.md](docs/for-ai.md)。**  
> 大多数看屏、读值、点按钮、开 Windows 功能的需求，直接调 `yx` 就行，不用写代码。

你让 AI 看屏幕，它不用先截一张几 MB 的图再自己猜。YX 把当前屏幕压成一句话：

```
[L0] 窗口「另存为」| 可操作 3 项 | 无错误
```

需要细节时再 `yx zoom`，拿回原始 OCR 行和控件树。

## 现在能做什么

- `yx framing --level L0|L1|L2`：取一帧，按级别给视图
- `yx framing --fast`：跳过 UIA、单路 OCR，约 200ms
- `yx zoom --ref <ref>`：按引用回 L2，不重新截屏
- `yx find <文字>`：找文字，返回坐标和分数
- `yx find <文字> --fast --region x,y,w,h`：只认一块区域，更快
- `yx read --find <文字>`：只回命中行和附近几行，适合读余额/错误/状态
- `yx read --find <文字> --fast`：单路 OCR，读值更快
- `yx click-text <文字>`：找到就点，可加 `--expect` / `--expect-gone` 验结果
- `yx wait <文字>`：等文字出现，`--gone` 等它消失，`--interval` 调轮询
- `yx status`：服务在不在、版本、管道、数据目录
- `yx words` / `yx words --json`：列出 30+ 个常用 Windows 命令词
- `yx words pending / accept / reject`：重复记忆——同一动作第 5 次成功会提议一个词
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

## 输入模糊化

不用记英文词名，也不用每次写完整命令：

```powershell
.\yx.exe run 设备管理器      # 解析成 devmgmt
.\yx.exe run 当前用户        # 解析成 whoami
.\yx.exe open 设置           # 解析成 settings 再打开
.\yx.exe click 保存          # 自动走 click-text；click 100 200 仍是坐标点击
```

解析顺序：精确名字 → 精确别名/描述 → 唯一子串。歧义时返回候选，不猜。

## 重复记忆（试做）

同一个动作（比如反复 `read --find 余额`）第 5 次成功，会生成一个提议：

```powershell
yx words pending
yx words accept <fingerprint> balance
yx run balance
```

提议存在 `%LOCALAPPDATA%\yx\repeat.txt`，确认后的词存在 `words.user.txt`。
AI 不能自己确认；`accept` 必须由人执行。`reject` 后同一指纹不再提议。

## 定点读和点文字

```powershell
.\yx.exe find 余额
.\yx.exe read --find 余额          # 返回“充值余额”和下面的 ¥62.52
.\yx.exe click-text 保存 --expect "已保存"
.\yx.exe wait "完成" --timeout 10000
```

`find` 先走 UI Automation，认不出来再回 OCR；`read` 只回命中行和上下几行，
不会把整屏塞回来。`click-text --dry-run` 只报目标坐标，不点。

## 下载

Release 里下载 `YX-1.0.0.zip`，解压后直接运行 `yx.exe`。不需要安装、不需要管理员权限、不写注册表。
需要 Windows 10/11，并装好系统 OCR 的中文/英文语言包。

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
- 不是企业级平台：单机、本地、没有服务端；但 CLI、管道协议、账本、限制和测试都是完整的。
  它先给你自己的电脑用。

## 现在不做

文本压缩、AI 自动生成/确认词表、复杂记忆（分层/遗忘）、托盘、本地 HTTP、GPU OCR、账本的 HMAC/外部锚。
这些以后再说，v1.0 先把“一个 exe、能看见、能点、能查、能定点读、能记习惯、能听中文、能快”做稳。

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
