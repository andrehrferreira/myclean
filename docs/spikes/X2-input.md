# X2 — Input and clipboard (spike note)

**Status:** partly verified. Run on 2026-10-31 on Windows 11 Pro 10.0.26200,
Ryzen 9 7950X3D (32 logical CPUs), non-elevated, interactive session 1.
**Unblocks:** I1 (input watchdog), I2. **Requirements touched:** INP-1, INP-2,
INP-3 and the `input-watchdog` spec (Hook-Free Observation, Copy Latency,
Paste Stall, Incident Recording, Mechanism Classification).

> **Scope cut during the spike.** The owner was using the PC, so the
> coordinator stopped every interactive experiment before any had run. The
> rules from then on: no visible windows, no `SendInput`, no clipboard
> writes, no focus changes, no global hooks, and no recording of the owner's
> input. Everything below uses **message-only windows (`HWND_MESSAGE`)** and
> **read-only snapshots** only. Every question that needs injected keystrokes,
> a slow low-level hook or a test clipboard owner is marked
> **NOT VERIFIED**. It goes to a dedicated test session the owner starts
> (section 6).

## 1. Questions

- **(a)** On 26200, what is the effective default and cap of
  `LowLevelHooksTimeout`, and is a `WH_KEYBOARD_LL` hook that exceeds it
  silently removed?
- **(b)** Does `RegisterRawInputDevices(RIDEV_INPUTSINK)` deliver keyboard
  `WM_INPUT` to a background window without adding latency? How accurate is
  `GetMessageTime` compared with `QueryPerformanceCounter` (QPC)?
- **(c)** Can `AddClipboardFormatListener`, `GetClipboardOwner`,
  `GetOpenClipboardWindow`, `GetClipboardViewer` and
  `SendMessageTimeout(WM_NULL)` produce the incident data the spec asks for,
  and can copy latency be measured end to end?
- Record which observations PowerToys Keyboard Manager and AnyDesk
  influence, without stopping them.

## 2. Method

The experimental code lives outside the repo, in
`C:\Users\<user>\AppData\Local\Temp\claude\f--MyClean\c9ddaf7d-862d-42f1-a36e-af1fa4e3176b\scratchpad\spikes\`:

- `x2.cpp`: a single test program.
- `build.ps1`: enters the VS 2022 Build Tools dev shell (x64) and runs
  `cl /nologo /EHsc /O2 /std:c++20 /W3 /DUNICODE /D_UNICODE x2.cpp /link user32.lib …`.
- `x2-results.txt`, `x2-register.txt`: raw outputs.

```text
powershell -NoProfile -File build.ps1 x2.cpp
x2.exe all        # clocks + probe semantics + clipboard snapshot
x2.exe register   # listener / raw-input registration check
```

What each mode does:

| Mode | Experiment |
|---|---|
| `clocks` | Reads the current timer resolution (`NtQueryTimerResolution`). Records the step sizes of `GetTickCount` over 1 s. A second thread posts 300 `WM_APP` messages to a message-only window at 1–7 ms intervals, stamping `GetTickCount()` and QPC at each post. The receiver compares `GetMessageTime()` with both stamps and measures queue delay with QPC. |
| `probes` | Four threads, each with its own message-only window: *responsive* (pumps), *hung* (stops pumping for 6.5 s), *owner* (handles a sent `WM_USER+1` by sleeping 800 ms) and *paster* (blocks in a cross-thread `SendMessage` to *owner*, which simulates `GetClipboardData` waiting on `WM_RENDERFORMAT`). Each is probed with `SendMessageTimeout(WM_NULL, 100 ms)`; `IsHungAppWindow` is read at about 1.7 s and 5.8 s. |
| `snapshot` | One read-only snapshot: `GetClipboardSequenceNumber`, `GetClipboardOwner` (process image, whether the owner is message-only, `IsHungAppWindow`, one `WM_NULL` probe), `GetOpenClipboardWindow`, `GetClipboardViewer`, `CountClipboardFormats`, `GetUpdatedClipboardFormats`. It never calls `OpenClipboard` and never reads data. |
| `register` | On a message-only window: `AddClipboardFormatListener` then `RemoveClipboardFormatListener`; `RegisterRawInputDevices({0x01,0x06,RIDEV_INPUTSINK,hwnd})`, confirmed with `GetRegisteredRawInputDevices`, then `RIDEV_REMOVE`. No message is pumped while registered; any queued `WM_INPUT` is discarded unread. |

Environment, read with PowerShell (read-only):
`HKCU\Control Panel\Desktop\LowLevelHooksTimeout`,
`HKCU\Software\Microsoft\Clipboard`, the `cbdhsvc_*` service,
`Get-Process AnyDesk, PowerToys*` (session IDs),
`%LOCALAPPDATA%\Microsoft\PowerToys\settings.json` and the Keyboard Manager
folder.

## 3. Results

### 3.1 Environment

| Item | Observed |
|---|---|
| `LowLevelHooksTimeout` (HKCU) | **absent**, so the system default applies |
| PowerToys | v0.51.1. Enabled modules: Awake, ColorPicker, FancyZones, File Explorer, FindMyMouse, Image Resizer, **Keyboard Manager**, MouseHighlighter, PowerRename, PowerToys Run, Shortcut Guide, Video Conference |
| `PowerToys.KeyboardManagerEngine` | running in session 1 since 2026-09-30 09:25. **No remap file found**: the Keyboard Manager folder holds only `Engine\` and `ModuleInterface\` log folders. The engine installs its low-level hook anyway, apparently with an empty remap table |
| AnyDesk | one process, **session 0** (the service). No AnyDesk process in session 1 at test time |
| Clipboard history | `HKCU\Software\Microsoft\Clipboard` has no `EnableClipboardHistory` or `EnableCloudClipboard` value (Windows default: off). `cbdhsvc_dc055f` service: Running / Automatic |
| Machine load during the spike | 98–100 % total CPU for over an hour (other agents' builds and linters). This is a realistic stress backdrop for the timings below |

### 3.2 Clocks (`x2 clocks`)

| Measurement | Result |
|---|---|
| System timer resolution | 1.000 ms right now (raised by some process; coarsest 15.625, finest 0.5) |
| `GetTickCount` step sizes over 1 s | **15 ms ×24, 16 ms ×40**. It ignores the 1 ms timer resolution |
| `GetMessageTime` − `GetTickCount()` at post | p50 0, p99 0, max 16 ms. **`GetMessageTime` is the tick count at post time** |
| `GetMessageTime` error vs the true post time (QPC) | mean −5.97 ms, p50 −5.26, min −15.59, max +1.60 ms |
| Queue delay `PostMessage` → `GetMessage` (QPC), under about 100 % CPU load | mean 0.26 ms, p50 0.12, p99 1.59, max 3.42 ms |

### 3.3 `SendMessageTimeout(WM_NULL, 100 ms)` semantics (`x2 probes`)

| Target | Result |
|---|---|
| Responsive window | answered in 0.142 ms |
| **Paster blocked inside a cross-thread `SendMessage` to a slow owner** | **answered in 0.060 ms** |
| Owner busy in its 800 ms handler | no answer, `ERROR_TIMEOUT` (1460) after 106.7 ms |
| Thread not pumping for about 1.6 s | no answer, 1460 after 114.7 ms; `IsHungAppWindow` = 0 |
| Thread not pumping for about 5.7 s, with `SMTO_ABORTIFHUNG` | no answer, 1460 after **105.1 ms** (the flag did not shortcut the wait); `IsHungAppWindow` = **1** at about 5.8 s, also for a message-only window |

### 3.4 Clipboard snapshot (`x2 snapshot`)

| Field | Value |
|---|---|
| Sequence number | 15451 |
| `GetClipboardOwner` | `Code.exe` (VS Code). The owner window is **message-only**; `IsHungAppWindow` = 0; `WM_NULL` answered in 0.108 ms |
| `GetOpenClipboardWindow` | none |
| `GetClipboardViewer` (legacy chain head) | **none** |
| Formats | `GetUpdatedClipboardFormats` works **without `OpenClipboard`**: 13 (CF_UNICODETEXT), 16 (CF_LOCALE), 1 (CF_TEXT), 7 (CF_OEMTEXT). The last two are synthesized by Windows |

### 3.5 Registration (`x2 register`)

| Call (on an `HWND_MESSAGE` window) | Result |
|---|---|
| `AddClipboardFormatListener` | OK, 0.144 ms; removal OK |
| `RegisterRawInputDevices(keyboard, RIDEV_INPUTSINK)` | OK, 0.148 ms; `GetRegisteredRawInputDevices` shows flags 0x100 targeting our window; `RIDEV_REMOVE` OK |

Antivirus: the machine's active product is **Kaspersky** (Defender is
passive). It did not block `x2.exe`, including the `RIDEV_INPUTSINK`
registration. It did quarantine an X4 test binary; see X4 §3.8.

## 4. Conclusions per question

**(a) Low-level hook timeout and removal: NOT VERIFIED.** Known facts:
`LowLevelHooksTimeout` is absent on this machine, so the built-in default
applies. Microsoft's `LowLevelKeyboardProc` documentation (not re-measured
here) caps the effective value at 1000 ms on Windows 10 1709 and later. It
also says a hook that times out is skipped, and on Windows 7 and later is
silently removed without notice. The built-in default when the value is
absent, and how many timeouts trigger removal, are **not documented**. Do
not guess them; measure them in the dedicated session (§6, test T1).

**(b) Raw input and timestamps: partly verified.**
- Registration with `RIDEV_INPUTSINK` works on a message-only window, so the
  tray needs no visible or focusable window.
- **Not verified:** that `WM_INPUT` actually arrives while another
  process has focus, and its latency relative to the foreground app.
- `GetMessageTime` is **unusable for latency**. It is `GetTickCount`, with
  15–16 ms steps and about −6 ms average error, even while the system timer
  runs at 1 ms. **QPC taken when the message is received is accurate**: the
  queue delay is 0.12 ms p50 and 1.6 ms p99 under full CPU load. The error
  is far below the 250 ms incident threshold, but not negligible for the
  spec's "about 40 ms" copy example.
- **Open question that matters for INP-3:** Windows probably calls
  low-level hooks before it generates raw input (likely, NOT VERIFIED). If
  so, a Ctrl+C `WM_INPUT`
  arrives *after* any hook delay, and a copy latency timed from `WM_INPUT`
  will **not** include the hook delay. The slow-hook mechanism would then be
  invisible to the copy timer. However, the input event's own time stamp
  (`GetMessageTime()` of the `WM_INPUT`, the same clock as
  `KBDLLHOOKSTRUCT.time`) may be the event time, not the delivery time. In
  that case `QPC-at-receipt − event time` *is* the hook delay, at 16 ms
  resolution. This is test T2 in §6 and decides how the slow-hook mechanism
  can be classified.

**(c) Clipboard incident data: partly verified.**
- Gathered **without opening the clipboard**: owner (window, process, whether
  it is message-only, hung state, `WM_NULL` responsiveness), open-clipboard
  holder, viewer-chain head, and the format list (`GetUpdatedClipboardFormats`).
- **Format sizes cannot be gathered passively.** Sizes need `OpenClipboard`
  plus `GetClipboardData` plus `GlobalSize`. That (1) makes the owner render
  delayed formats (`WM_RENDERFORMAT`) and synthesized formats, (2) holds the
  clipboard open from MyClean, which is itself mechanism b for every other
  app, (3) can block the watchdog on a hung owner, and (4) maps clipboard
  content into MyClean's memory.
- **Probe finding (simulated with plain `SendMessage` between message-only
  windows):** a thread blocked waiting on another thread's `SendMessage`
  still answers `WM_NULL` in 0.06 ms, because Windows dispatches incoming
  sent messages while it waits. Only the busy *owner* fails the probe. If
  `GetClipboardData`'s internal `WM_RENDERFORMAT` wait behaves the same
  (likely, but **NOT VERIFIED**, test T4), probing **only the foreground
  window** will report a near-zero paste stall during a delayed-rendering
  freeze. The paster's UI is visibly frozen (it does no painting), yet it
  answers `WM_NULL`.
- End-to-end copy latency (injected Ctrl+C → `WM_CLIPBOARDUPDATE`): **NOT
  VERIFIED**, test T3.

**PowerToys and AnyDesk influence on what was observed:**
- Keyboard Manager is running and hooks every key, but has no remaps
  configured. Its only possible effect is mechanism d (slow hook), which was
  not measurable here.
- AnyDesk ran only as a session-0 service. A session-0 process cannot open
  the session-1 clipboard. The empty `GetOpenClipboardWindow` and viewer
  chain were therefore not influenced by AnyDesk. AnyDesk becomes relevant
  only while a remote session runs its user-session process.
- Neither tool influenced the clock or probe measurements, which used only
  our own message-only windows.

## 5. Implications for the product spec

1. **Paste Stall (input-watchdog spec):** probing only the foreground window
   misses delayed-rendering stalls (§4c). Change: on Ctrl+V, probe **both**
   the foreground window and the clipboard owner (`GetClipboardOwner`, when
   it is in a different thread) every 100 ms for up to 5 s. Record two
   numbers: *foreground unanswered time* and *owner unanswered time*. Keep
   "paste stall" as the foreground figure, and treat an owner that does not
   answer within the paste window as the delayed-rendering fingerprint for
   Mechanism Classification. Also record whether the foreground window
   stopped painting, if cheap; otherwise state the limitation in the
   Freezes view.
2. **Copy Latency:** timestamp both ends with **QPC at message receipt**,
   never with `GetMessageTime`, which has 15–16 ms resolution. Run the
   watchdog's message thread at above-normal priority; this run's receive
   delay was ≤ 1.6 ms p99 under 100 % CPU at normal priority.
3. **Incident Recording:** "clipboard formats **and their sizes**" conflicts
   with the privacy requirement and with not causing mechanisms a/b. Change:
   record format IDs and names from `GetUpdatedClipboardFormats` (no
   `OpenClipboard`). Drop sizes, or make them an explicit owner opt-in. The
   heavy-payload mechanism should then key on the *format set* (for example
   CF_DIB/CF_DIBV5/PNG/`HTML Format` present) plus copy latency, not on byte
   size. Also mark synthesized formats (CF_TEXT/CF_OEMTEXT next to
   CF_UNICODETEXT) so they are not counted twice.
4. **Hook-Free Observation:** confirmed workable on `HWND_MESSAGE` windows
   for both the raw input sink and the clipboard listener. Both registrations
   take about 0.15 ms. Two additions:
   - The raw input sink hands **every** keystroke to the tray process. The
     "Only Copy and Paste Timing Recorded" rule must be enforced at the
     `WM_INPUT` handler: track only modifier state plus C/V make codes,
     discard everything else before any buffer or log, and add a unit test
     for it.
   - A long-running `RIDEV_INPUTSINK` looks like keylogger behaviour to
     antivirus heuristics. Kaspersky did not react to the 0.15 ms
     registration, but the signed tray build must be checked with Kaspersky
     before M1 exit (see X4 §3.8 for an actual quarantine).
5. **Mechanism Classification:**
   - `IsHungAppWindow` flips only after about 5 s without pumping, including
     for message-only windows. Use it for the "hung top-level windows" field,
     not for classifying sub-5 s stalls; the 100 ms `WM_NULL` probe is the
     signal there.
   - `SMTO_ABORTIFHUNG` did not shorten a probe to a hung window (105 ms), so
     the probe loop's cost per hung target is the full timeout. Probe targets
     sequentially within the 100 ms period, or one thread per target.
   - The slow-keyboard-hook fingerprint depends on test T2. Until then,
     classify mechanism d only from "typing lag outside copy/paste", per F-026.
6. **F-029 suspect ranking needs data, not the current rationale:**
   - Keyboard Manager has **no Ctrl remaps** (no remap file at all) on this
     machine, though its hook still sees every key.
   - AnyDesk is service-only while no session is active.
   - Clipboard history is off by default (no HKCU value); `cbdhsvc` runs anyway.
   - The legacy viewer chain is empty, so mechanism c cannot currently occur.

   A new candidate worth recording in incidents: **sustained system CPU
   saturation**, which was seen for over an hour during this spike. A
   normal-priority low-level hook process (Keyboard Manager) competing with
   saturated builds is a plausible source of hook delay. This links I1 to the
   governor. Add total CPU and pressure state to each incident.
7. **Architecture (`IClipboardProbe`):** the adapter needs
   `GetUpdatedClipboardFormats`, not `EnumClipboardFormats`, which requires
   `OpenClipboard`.

## 6. Dedicated test session (owner starts it; nothing here has run)

These tests need injected input, a deliberately slow hook, or clipboard
writes, so they run only when the owner starts a session and leaves the PC
alone for about 5 minutes. The owner's clipboard is overwritten during
T3/T4.

| Test | Design | Settles |
|---|---|---|
| **T1 hook timeout and removal** | Process H installs `WH_KEYBOARD_LL`. It sleeps *D* ms only for injected `VK_F24` keys (`LLKHF_INJECTED`) and passes every real key through at once. Process R owns a foreground window and timestamps `WM_KEYDOWN` with QPC. A driver sends `VK_F24` with `SendInput`, 20 times per *D* ∈ {100, 200, 300, 500, 800, 1500} ms, and records send→`WM_KEYDOWN` latency, hook call count and the time of the first key that no longer reaches the hook. | Effective default timeout (latency plateaus at the cap); number of timeouts before silent removal; whether `UnhookWindowsHookEx` then fails |
| **T2 raw input vs hooks** | Same rig, plus a message-only `RIDEV_INPUTSINK` window in a third process. For each key, compare QPC at `WM_INPUT` receipt, `GetMessageTime()` of the `WM_INPUT`, and QPC at `SendInput`, with *D* = 0 and *D* = 300. Run once with no sink to compare R's latency. | Whether `WM_INPUT` comes after hook delay; whether its message time is the event time (a hook-delay detector); that the sink adds no latency |
| **T3 copy latency end to end** | A test window owning the clipboard handles Ctrl+C by calling `SetClipboardData` after an artificial 0/40/400 ms delay. The watchdog prototype (raw input sink + listener, message-only) measures Ctrl+C `WM_INPUT` → `WM_CLIPBOARDUPDATE` with QPC. The driver injects Ctrl+C. The owner's text clipboard is saved and restored. | Copy latency accuracy against the known delay; who opens the clipboard right after an update (poll `GetOpenClipboardWindow` for 500 ms: `cbdhsvc`, AnyDesk when a session is active, PowerToys) |
| **T4 delayed-rendering paste** | The owner sets a delayed-render format (`SetClipboardData(fmt, NULL)`) and sleeps 800 ms in `WM_RENDERFORMAT`. A paster window calls `GetClipboardData`. The prober sends `WM_NULL` (100 ms) to the paster and the owner every 100 ms. | Whether the real `GetClipboardData` wait answers `WM_NULL` the way the simulated wait did (§3.3), which decides change 1 in §5 |

To avoid touching the owner's desktop, T3 and T4 can also run in a private
non-interactive window station (`CreateWindowStation` + `CreateDesktop`),
which has its own clipboard. That variant does not show the influence of
`cbdhsvc`, AnyDesk or PowerToys. It was **not attempted**, because it
involves clipboard writes, which the restriction forbade.
