# 提速方案（v0.7.0 已实施第一版）

状态：2026-10-23。`--region`、`--fast`、`wait --interval`、`framing --fast`、耗时字段、
Job 超时生命周期修复已落地；`--max-age` 帧复用留到下一版。

目标：把两件事同时提速，不扩架构、不做大项目。

## 一、我干活的速度

- 一轮只做一个用户可见功能，不顺手重构。
- 批量改完再构建，构建一次、测试一次；GUI 只在最后验。
- 实现细节我自己定；只有公开、不可逆、花钱的操作才问。
- 先给结果，再给一句原因；不写长设计稿。
- 时间盒：一个功能超过预定次数工具调用，就停下报告卡点。

## 二、yx 的响应速度

现在 `framing / find / read / click-text / wait` 都是全屏 OCR，约 400ms 一次。

已加：

- `--region x,y,w,h`：只识别目标区域。
- `--fast`：单路 OCR（中文 1x），跳过 UIA 和英文第二遍。
- `wait --interval <ms>`：默认 500ms，不再 250ms 全屏轮询。
- `framing --fast`：L0 只走单路 OCR，跳过 UIA。
- JSON 里的 `ms` / `ocr_ms` / `uia_ms`：先测再优化；不追 GPU。

预期：全屏约 400ms，缩区域或 fast 后约 100 到 200ms。

## 三、验收

- 用 `tests/corpus/sample.png` 做固定样本，不依赖当前屏幕。
- 对比 `find` / `read` / `click-text --dry-run` 在默认、`--fast`、`--region` 下的 `ms`。
- 现有 16 项测试继续全过。

## 四、暂不做

- 不改成多线程 OCR/UIA。
- 不做服务端自动缓存；`--max-age` 留到下一版。
- 不换 OCR 引擎，不加 GPU。
- 不改词表结构、重复记忆策略和账本格式。
