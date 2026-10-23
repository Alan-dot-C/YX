# 给 AI 的一页

你是 AI。需要看屏幕、读文字、点按钮、开 Windows 功能时，**先别写 OCR / UI 自动化代码**，先调用本机的 `yx.exe`。它已经把采集、投影、点击、记账、常用词都包好了。

## 第一次先自报能力

```powershell
yx schema
yx words --json
yx help <词>
```

- `yx schema`：机器可读的能力清单（ops、levels、exit_codes）。
- `yx words --json`：Windows 命令词表（name / category / desc / kind）。
- `yx help 设备管理器`：单词说明；中文别名和描述也能匹配。

## 看

```powershell
yx framing --level L0          # 一句话：窗口/对话框、可操作项、状态
yx framing --level L0 --fast   # 跳过 UIA、单路 OCR；约 200ms
yx framing --level L1          # 区块 + 控件 + 粗位置
yx framing --level L2          # 原始 OCR 行 + UIA + 窗口
yx zoom --ref <ref>            # 回 L2，不重新截屏
yx find 余额                   # 命中文字 + 坐标 + 分数
yx read --find 余额            # 只回命中行和附近行，读具体值用这个
yx read --find 余额 --fast --region 300,300,800,300   # 只盯一块区域，快
```

- `read --find` 比 `framing L0` 更适合读余额、错误码、状态值。
- 目标窗口不在前台时，先 `yx focus "<标题片段>"`。
- 复杂界面先 L0，再 `zoom`，不要一上来 L2 全量。
- `--fast` 只跑中文单路 OCR，会牺牲英文和数字精度；`--region` 会漏掉区域外文字。两者都是显式选项，默认仍走准确路径。
- 结果里的 `ms` 是这次调用耗时，`ocr_ms` / `uia_ms` 是分项；慢的时候先看这个。

## 做

```powershell
yx click 保存                          # 非坐标自动走 click-text
yx click 100 200                       # 两个数字 = 坐标点击
yx click 保存 --expect "已保存"        # 点完等文字出现
yx click 保存 --expect-gone "保存"     # 点完等文字消失
yx wait "完成" --timeout 10000         # 等文字出现
yx wait "正在处理" --gone              # 等文字消失
yx wait "完成" --timeout 10000 --interval 500 --fast   # 快轮询，单路 OCR
yx open 设备管理器                     # 中文名/URL/路径都能开
yx run devmgmt                         # 打开设备管理器
yx run ipconfig                        # 跑只读命令，stdout 在 JSON 里
```

- `click` / `click-text` 没有撤销；不确定时先加 `--dry-run` 看坐标。
- 不要自己拼 shell 命令；优先用 `yx run <词>`。
- 危险命令（格式化、关机、分区、删注册表）没有预置。

## 重复记忆

```powershell
yx words pending
yx words accept <fingerprint> <name>
yx words reject <fingerprint>
yx run <name>
```

同一个 `read` / `click-text` 第 5 次成功，会生成一个 `proposed` 提议。
**AI 不要自己 accept**；让用户确认。确认后，下次只发一个词。

## 非 CLI 客户端

命名管道：

```
\\.\pipe\yx-<用户名>
```

请求/响应都是 `u32` 长度前缀 + UTF-8 JSON。字段和示例见
[docs/protocol.md](protocol.md)。

## 结果怎么读

所有命令输出单个 JSON 对象：

- `ok`：是否成功。
- `rc`：失败时的退出码。
- `error`：人可读的错误。
- `stdout` / `stderr`：`yx run` 的命令输出。
- `text` / `lines` / `matches`：视图和命中。

退出码：

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 参数错误、未知词、歧义 |
| 3 | 采集或执行失败 |
| 4 | 超时 / ref 过期 |
| 5 | 后条件验证失败 |
| 6 | 服务未就绪（客户端本地回退） |

## 不要做

- 不要为了看屏写 OCR 代码；先 `yx framing` / `yx read`。
- 不要把整张截图塞回模型；先 `yx read --find`。
- 不要用像素坐标硬编码；先 `yx find` 或 `yx click-text`。
- 不要自动接受学习词，不要执行危险命令。
- 不要把 YX 当成全自动 Agent；它负责感官和记账，理解和决定仍然是你。

## 限制

- 只支持 Windows 10/11，需要系统 OCR 语言包。
- `find` / `read` / `wait` 读当前前台屏；先 focus。
- OCR 数字可能认错；点击前先 `--dry-run`。
- 账本只能发现误改/局部改，不是签名，不是“改不掉”。
