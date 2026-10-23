# Changelog

## 1.0.0 — 2026-10-23

第一个正式本地版。一个 `yx.exe`，器官层和中枢层合并，保留 `handeye.bat` 兼容。

- 观测：`framing` L0/L1/L2、`zoom`、`find`、`read --find`、`click-text`、`wait`
- 快模式：`--fast`、`--region`、`wait --interval`、`framing --fast`，JSON 带耗时
- 输入模糊化：中文别名/描述唯一匹配；`yx open 设置`；`yx click 保存` 自动走文字点击
- 重复记忆：`read` / `click-text` 第 5 次成功生成提议；`words pending/accept/reject`
- 安全边界：词表人工审核；账本 FNV-1a 链 + `head` 锚；`log verify`
- 稳定性：修复 Job 15 秒超时后的 use-after-free；`words accept` 只接受 `proposed`
- 测试：16 项，固定图片样本，测试不写学习数据（`YX_NO_LEARN=1`）

## 0.7.0

- fast mode、耗时字段、Job 生命周期修复。

## 0.6.0

- 输入模糊化：中文别名、`yx open`、`yx click` 路由。

## 0.5.0

- 重复记忆试做版。

## 0.4.0

- `find`、`read --find`、`click-text`、`wait`。

## 0.3.0

- 合并成一个 `yx.exe`，保留 `handeye.bat`。

## 0.2.0

- 词表从 146 砍到 32。

## 0.1.0

- 第一版：framing、zoom、账本。
