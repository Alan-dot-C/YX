# YX architecture: three layers, three contracts

YX is not one program that does everything. It is a small organism:

```
AI client            the cortex: understands, plans, decides
   |
   v
YX                   thalamus + hippocampus + ledger
   |
   v
handeye              retina + spinal cord + hands + mouth
```

The biological words are not decoration. They decide what each layer is
allowed to do. A layer that starts doing another layer's job becomes the
second brain, and then nothing can be audited.

## Layer responsibilities

### handeye 1.2 — organs

Does:

- capture the screen, OCR, read the UI Automation tree, enumerate windows,
  read the cursor, foreground window and clipboard;
- move the mouse, click, type, press keys, focus windows, run low-level
  scripts, record/replay, speak, stop;
- keep local reflexes: dangerous-word guard, stop event, max duration,
  fail-open when YX is not running.

Does not:

- clean, merge, classify, rank, or summarize OCR lines;
- keep cross-call state;
- verify that an action worked;
- learn words, remember anything, or write the audit ledger.

### YX — middle layer

Does:

- turn one capture into L0 / L1 / L2 projections;
- keep a short per-client working memory (previous frame, attention region,
  `ref` cache);
- extract the alarm channel (dialogs, error words, dialog buttons, numeric
  changes, new windows) before any truncation;
- keep the audit ledger and verify its hash chain;
- expose a stable JSON contract over a named pipe and a CLI.

Does not:

- understand what the screen means;
- decide what to click;
- move the mouse or type;
- summarize or rewrite text.

### AI client — cortex

Does:

- understand the projected frame;
- decide the next action;
- ask YX for a closer view (`zoom`) or a fresh frame (`framing`);
- call handeye through YX for actions;
- read and verify the ledger.

## Three contracts

1. **One-way dependency.** AI → YX → handeye. handeye never calls YX. A
   callback from the organ to the middle layer creates a loop: YX waits for a
   handeye subprocess while that subprocess waits for YX. This is forbidden.

2. **Raw at the boundary.** handeye gives YX raw structured data: OCR lines
   with boxes and confidence, UI Automation nodes with roles and rects, window
   rectangles. YX gives the client a projection. The client never receives a
   summary invented by a model.

3. **The alarm channel is sacred.** A projection may drop ordinary lines to
   fit a budget. It may not drop dialogs, error words, dialog buttons, lines
   whose numbers changed since the previous frame, or a newly appeared window.
   If the alarm lines alone exceed the budget, the budget loses and the output
   records `alarm_overflow`.

## Fail-open exception

handeye keeps a local reflex path so the machine is not paralyzed when YX is
down. That is the only case where handeye is allowed to do something that
looks like processing: a fast local guard/stop, and a local read-only check
needed to avoid a blind action. It is intentionally small and not part of the
public projection contract.

## Current status

- The organ layer (`handeye 1.2`) is trimmed to raw collection and execution,
  and is now linked into the same `yx.exe` binary. `handeye.bat` forwards old
  command names for compatibility.
- `yx 0.3` has `serve`, `status`, `framing` (L0/L1/L2), `zoom`,
  `log verify` (FNV-1a chain plus a `logs\head` anchor), and a curated
  `words` table with `words` / `help` / `run` / `schema`.
- The word table is human-curated. `open` is an in-process `HandeyeOpen`;
  `exec` runs a fixed `cmd.exe` command and captures its output. AI cannot add
  words.
- The ledger is tamper-evident: it detects edits and truncation, but it is not
  a signature and not tamper-proof. See `docs/limitations.md`.
- `watch`, text compression, memory, tray, HTTP, and GPU are postponed.
