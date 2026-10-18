# Known limitations (v0.1)

This is a small, local tool. It is not a production security product.

## Ledger

- The chain is `FNV-1a 64` over `previous chain || line body`. It detects an
  accidental edit or a local change: the first mismatch is reported by file
  and line number.
- `logs\head` stores the last chain value and the number of chained lines. It
  is what lets `yx log verify` notice that the last line was deleted.
- The head file can be edited by the same user. If both the log and the head
  are rewritten consistently, verification passes. This is **tamper-evident,
  not tamper-proof**.
- There is no HMAC and no external anchor. Someone who can rewrite the whole
  log can recompute the whole chain.
- Crash-flight records are skipped when the chain is enabled, because the
  crash path cannot safely take the log mutex. A hard crash can therefore lose
  the last few in-memory lines.
- Legacy lines without `chain=` are counted as `unverified` and do not fail
  verification. New YX ledgers are fully chained.

## Framing

- L0 is deterministic but heuristic. It can be wrong on custom-drawn UIs,
  games, remote desktops, and apps with poor UI Automation trees.
- The alarm channel keeps dialogs, error words, dialog buttons, and newly
  appeared windows before ordinary lines are reduced. It is conservative, so
  L0 sometimes has more than one line.
- `ref` points into a 16-frame in-memory cache. An old ref can be evicted;
  `zoom` then returns rc 4. There is no 20-second timer yet.
- L2 is capped by the pipe message size (1 MB) in the transport; a very dense
  screen can be truncated.

## Platform

- Windows 10/11 only.
- OCR uses the Windows language packs. Install the Chinese and/or English OCR
  language pack or `handeye ocr` returns an error.
- UI Automation quality varies by application. Some apps expose almost
  nothing; YX falls back to OCR only.
- Per-monitor DPI is set by YX/handeye at process start. A client that
  captures coordinates from another DPI context may need to scale them.

## Not in v0.1

- Text-file compression
- Word/skill learning and memory
- Tray icon, local HTTP, Event Viewer integration
- GPU OCR
- `watch` / server push
- A signed or externally anchored ledger
