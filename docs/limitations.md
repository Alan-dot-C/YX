# Known limitations (v0.7)

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

## Words

- The word table is curated and compiled in. Adding a word requires a rebuild;
  AI cannot add words at runtime.
- `open` targets depend on the Windows edition and version. `gpedit` and
  `secpol` are not present on Home; some `ms-settings:` pages differ between
  builds.
- `exec` runs through `cmd.exe`, captures at most 64 KB of stdout/stderr, and
  times out after 30 seconds. A non-zero exit code is reported as-is; it does
  not always mean the command is broken.
- There is no argument substitution. Words are fixed commands, not templates.

## Fast mode

- `--fast` runs Chinese 1x OCR only and skips the English second pass; it is
  worse on Latin text and numbers.
- `--fast` on `find` / `click-text` / `framing` also skips UIA; custom controls
  that only exist in the accessibility tree may disappear.
- `--region` ignores everything outside the rectangle. If the target moves out,
  the command will not see it.
- `wait --interval` can miss short-lived text between polls. `--fast` makes OCR
  cheaper but not more accurate.
- Fast mode is opt-in; the default path keeps the old behavior.

## Input fuzzification

- It is string matching, not language understanding: exact name, exact
  alias/description, then unique substring. There is no model and no semantic
  similarity.
- Most Chinese queries work through the description; only a few words have an
  explicit alias (for example `设置`).
- `click` treats two integer arguments as coordinates; everything else becomes
  `click-text`. `1e3` is not treated as a coordinate.
- `open` treats anything with `:`, `/`, `\`, or an executable extension as a
  direct target. Ambiguous word queries return candidates instead of guessing.
- `wait` and `find` still read the current foreground screen.

## Repetition memory

- Only `read` and `click-text` successes are recorded. Arbitrary shell
  commands and `find` / `wait` are not.
- The fingerprint is exact after normalization: `余额` and `余额 ` merge, but
  `余额` and `账户余额` do not.
- A proposal only appears after five successes; there is no decay, archive, or
  degraded state yet.
- A learned `read` word stores the image path if the original action used
  `--image`. If that file is deleted, the learned word fails.
- `accept` / `reject` are human commands. There is no tray prompt yet.

## Task commands

- `find` / `read` / `wait` read the current foreground screen. Focus the
  target window first; they do not take a window handle yet.
- `find` / `read` are OCR fuzzy. Numbers can be misread (`62.52` can come back
  as `62 · 52`); always treat the text as a hint, not a typed value.
- `read --find` returns lines by vertical proximity and column overlap, not by
  semantic grouping. It can include a neighbouring label.
- `click-text` can click the wrong target if the same text appears more than
  once. Use `--index`, or `--dry-run` first and inspect the coordinates.
- There is no undo. A click is a real mouse event.
- `wait` polls the whole screen every 250 ms; there is no region filter yet.

## Platform

- Windows 10/11 only.
- OCR uses the Windows language packs. Install the Chinese and/or English OCR
  language pack or `handeye ocr` returns an error.
- UI Automation quality varies by application. Some apps expose almost
  nothing; YX falls back to OCR only.
- Per-monitor DPI is set by YX/handeye at process start. A client that
  captures coordinates from another DPI context may need to scale them.

## Not in v0.6

- Text-file compression
- AI-generated words and memory
- Tray icon, local HTTP, Event Viewer integration
- GPU OCR
- `watch` / server push
- A signed or externally anchored ledger
