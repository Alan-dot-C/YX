# YX

**A local perception and audit layer for AI agents.**

YX sits between an AI client and Windows. It lets the client ask *"what is on
screen?"*, *"zoom into that"*, and *"show me what happened"* through a small,
stable JSON contract. It does not decide, summarize, or move the mouse.

The goal is not another GUI agent. The goal is a **middle layer** that any
agent can call: DSH, Codex, a local script, or something not written yet.

> Status: **v0.1 in development.** The repository currently contains the
> clean scaffold and the first `yx status` server/client. `framing`, `zoom`,
> and the ledger are the next commits. Do not treat this snapshot as a
> finished release.

## Two layers

```
AI client (DSH, Codex, custom)   understands, decides
             |
             |  CLI / named pipe / JSON
             v
YX          thalamus + hippocampus + ledger
             attention, working memory, audit
             |
             v
handeye     senses + hands
             screen capture, OCR, UIA, mouse, keyboard
```

- **handeye 1.2** is the organ layer: capture and execute only. It keeps raw
  output and local reflexes; it does not sieve, rank, summarize, or verify.
- **YX** is the middle layer: it gates attention, projects a frame into L0/L1,
  caches recent frames, and keeps a tamper-evident log. It never understands
  the screen and never acts.
- The AI is the part that understands. YX exists to make its senses cheap and
  its actions auditable.

## Why another layer

Most GUI agents are end-to-end: screenshot in, action out. That is expensive,
non-deterministic, and impossible to audit. YX takes the opposite route:

- **Projection, not summary.** Same input, same output. No model in the loop.
- **Alarm channel never downsampled.** Dialogs, error words, dialog buttons,
  numeric changes, and new windows survive every compression level.
- **One place acts.** Only handeye moves the mouse or types.
- **One place records.** YX writes the ledger; the client can verify it.

## Build

Requirements:

- Windows 10/11
- Visual Studio 2022 with **Desktop development with C++**
- Windows OCR language pack for the languages you want to read
- No third-party libraries, no network access in the core path

From a normal PowerShell:

```powershell
.\build.ps1
```

Or from an **x64 Native Tools Command Prompt for VS**:

```bat
build.bat
```

Outputs:

```
handeye.exe
yx.exe
```

## Quick start

```powershell
# Check the senses and the environment
.\handeye.exe doctor

# Start the resident service (one per user)
.\yx.exe serve

# In another terminal: ask for status
.\yx.exe status

# Stop the service with Ctrl+C
```

`yx status` works even when the service is not running: it prints a local
fallback with `"server": false` instead of blocking.

## Any agent can call it

YX is deliberately not tied to one client. There are two entry points:

1. **CLI** — easiest for Codex, shell scripts, and humans:
   ```powershell
   .\yx.exe status
   ```
2. **Named pipe** — for long-running clients:
   ```
   \\.\pipe\yx-<user>
   ```
   Framing: `u32` length prefix + UTF-8 JSON, one request and one response per
   message. The exact schema will be frozen in `docs/protocol.md` with the
   first `framing` commit.

A client only needs to know the command names and the JSON fields. There is no
SDK, no Python dependency, and no DSH-specific code.

## Scope of v0.1

In:

- `yx status` — server state, local fallback.
- `yx framing` — L0 / L1 projections from one capture (next).
- `yx zoom` — return L2 from the cached frame, without recapturing (next).
- `yx log verify` — FNV-1a hash chain plus a `logs\head` anchor (next).
- `handeye` — raw capture / OCR / UIA and low-level actions.

Out:

- text-file compression
- word/skill learning and memory
- tray icon, local HTTP, Event Viewer integration
- GPU OCR
- any claim of being "tamper-proof"

These are intentionally postponed. v0.1 is the smallest thing that is useful
and can be published.

## Design documents

- [`docs/architecture.md`](docs/architecture.md) — layer contract and the
  rules each layer must not break.
- `docs/protocol.md` — pipe contract (coming with `framing`).
- `docs/limitations.md` — what the ledger can and cannot prove (coming with
  the ledger).

## License

MIT. See [`LICENSE`](LICENSE).
