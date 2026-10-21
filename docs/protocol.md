# YX protocol (v0.4)

Any AI client can use YX. There is no SDK and no vendor-specific dependency.

## Transport

Named pipe:

```
\\.\pipe\yx-<user>
```

Message framing:

```
request  = u32 little-endian length + UTF-8 JSON
response = u32 little-endian length + UTF-8 JSON
```

One request, one response. The pipe accepts multiple concurrent clients; the
server serialises state changes on one worker thread.

The pipe ACL allows only `SYSTEM` and the pipe owner.

## CLI equivalents

Every operation is also available as a command, which is the easiest way for a
shell-based agent:

```powershell
yx status
yx framing --level L0
yx zoom --ref 00000001
```

The CLI prints the same JSON object it would receive over the pipe.

## Operations

### status

```json
{"v":1,"op":"status","id":"c1"}
```

Response:

```json
{"v":"1","id":"c1","ok":true,"op":"status","tool":"yx","version":"0.1.0",
 "server":true,"pid":1234,"uptime_ms":890,"clients":1,"frames":1,
 "pipe":"\\\\.\\pipe\\yx-<user>",
 "data_dir":"%LOCALAPPDATA%\\yx"}
```

If the server is not running, the CLI returns a local object with
`"server":false` and exit code 0. It never blocks on a missing server.

### framing

```json
{"v":1,"op":"framing","id":"c1","key":"dsh","target":"screen","level":"L0"}
```

- `key` identifies the client's previous-frame stream. Use a different key per
  agent/session.
- `target` is `screen` in v0.1.
- `level` is `auto`, `L0`, `L1`, or `L2`. `auto` → `L0`.

Response (L0):

```json
{"v":"1","id":"c1","ok":true,"op":"framing","level":"L0","ref":"00000001",
 "cached":false,
 "text":"[L0] 窗口「另存为」| 可操作 3 项 | 无错误",
 "delta":{"added":0,"removed":0,"changed":false},
 "alarms":[],"line_count":48,"ui_nodes":6,"ms":406}
```

Response (L2) also contains:

```json
{
  "lines":[{"text":"...","x":0,"y":0,"w":0,"h":0,"q":90,"src":"zh1x"}],
  "ui":[{"name":"保存","role":"button","value":"","x":0,"y":0,"w":0,"h":0,
         "invokable":true}],
  "windows":[{"title":"...","x":0,"y":0,"w":0,"h":0,"z":0}]
}
```

`ref` is an 8-hex handle to the cached frame. The server keeps the most recent
16 frames; an old `ref` can be evicted.

### zoom

```json
{"v":1,"op":"zoom","id":"c1","ref":"00000001"}
```

Returns the cached frame at L2. It does **not** capture the screen again.
If the ref is missing or evicted:

```json
{"v":"1","id":"c1","ok":false,"op":"zoom","rc":4,
 "error":"ref not found or expired: 00000009"}
```

## Levels

| Level | Contents | Typical size |
|---|---|---|
| `L0` | one line: window/dialog type, title, actionable count, status | 1 line |
| `L1` | blocks + invokable controls + coarse zones | ~10 lines |
| `L2` | raw OCR lines, UIA nodes, window rectangles | full |

The alarm channel (dialogs, error words, dialog buttons, newly appeared
windows, numeric changes) is kept before ordinary lines are reduced. If the
alarms alone exceed the budget, the budget loses and `alarms` still carries
them.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | bad argument or unknown op |
| 3 | capture/OCR/exec failed |
| 4 | ref expired or request timed out |
| 5 | ledger verification failed |
| 6 | server not running (client-side only) |

## Task commands (CLI)

```powershell
yx find 余额
yx read --find 余额
yx click-text 保存 --expect "已保存"
yx wait "完成" --timeout 10000
```

`find` returns:

```json
{"ok":true,"op":"find","text":"余额","via":"ocr","count":2,
 "matches":[{"text":"充值余额","x":395,"y":378,"w":199,"h":12,
             "score":0.88,"via":"ocr"}]}
```

`read --find` returns only matching lines plus nearby lines in the same column:

```json
{"ok":true,"op":"read","find":"余额",
 "text":"充 值 余 额\n¥ 62 · 52","count":2,
 "lines":[{"text":"充 值 余 额","x":395,"y":378,"w":199,"h":12},
          {"text":"¥ 62 · 52","x":394,"y":414,"w":98,"h":23}]}
```

`click-text` first tries UI Automation, then OCR. `--dry-run` reports the
target without clicking. `--expect` / `--expect-gone` poll the screen after the
click and return exit code 5 on timeout. `wait` does the same polling without
clicking; `--gone` waits for disappearance.

## Words / run / schema (CLI)

```powershell
yx words --json
yx help devmgmt
yx run devmgmt
yx run ipconfig
yx schema
```

`yx words --json` returns an array:

```json
[{"name":"devmgmt","category":"工具","desc":"设备管理器","kind":"open"}]
```

`yx run` returns:

```json
{"v":"1","tool":"yx","op":"run","ok":true,"word":"ipconfig",
 "kind":"exec","exit":0,"timed_out":false,"ms":120,
 "stdout":"...","stderr":"","truncated":false}
```

`open` words launch a target through handeye. `exec` words run a fixed
`cmd.exe` command, capture up to 64 KB of output, and time out after 30
seconds. There is no argument substitution: a word is a fixed command, not a
template. Words are curated in the binary; AI cannot add them.

`yx schema` is the machine-readable capability list (`ops`, `levels`, word
count, exit codes).

## Ledger (CLI only in v0.2)

```powershell
yx log verify
yx log verify --dir C:\path\to\logs
```

Output:

```json
{"v":"1","tool":"yx","op":"log.verify","ok":true,
 "chained":3,"unverified":0,"head_ok":true}
```

On the first mismatch it returns exit code **5** and fills `file`, `line`,
`expected`, `actual`, and `error` (`chain mismatch`, `head mismatch`, or
`head missing`). There is no pipe operation for verification yet.

## Notes

- The protocol is intentionally small. Additions must be backward compatible:
  new fields are fine; changing the meaning of existing fields is not.
- The organ layer is inside the same `yx.exe`; `yx capture/ocr/...` are CLI
  commands, not pipe operations. `handeye.bat` forwards old command names.
