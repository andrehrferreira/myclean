# 04 — Input and clipboard freezes (Ctrl+C / Ctrl+V)

The owner sees the screen freeze for a moment on Ctrl+C or Ctrl+V. The
freezes are not logged as app hangs (F-008), so the cause must be captured
live, when it happens.

## Findings

### F-026 — Five different mechanisms can make copy/paste block
- **Evidence:** Win32 clipboard and input model.
- **Impact:** Each mechanism leaves a different fingerprint, so the
  diagnostic must tell them apart:

  | # | Mechanism | Who waits | Fingerprint |
  |---|---|---|---|
  | a | **Delayed rendering.** The copying app only promised the data. On paste, Windows sends it `WM_RENDERFORMAT`; if that app is busy or hung, the pasting app waits | the app pasting | Clipboard owner (`GetClipboardOwner`) is hung or slow (`SendMessageTimeout(WM_NULL)`) |
  | b | **Clipboard held open.** Another process keeps the clipboard open (sync tools, clipboard history, remote-desktop agents), so `OpenClipboard` fails and apps retry in a loop | copier or paster | `GetOpenClipboardWindow` returns a foreign window for a long time |
  | c | **Legacy viewer chain.** Apps registered with `SetClipboardViewer` receive `WM_DRAWCLIPBOARD` synchronously, one after another. A hung viewer stalls the copier. (Modern `AddClipboardFormatListener` listeners get a *posted* message and cannot block.) | the app copying | `GetClipboardViewer` is non-null; the stall happens on copy, not paste |
  | d | **Slow low-level keyboard hook.** Every keystroke, including Ctrl, C and V, passes through each `WH_KEYBOARD_LL` hook before reaching any app. A slow hook delays all input, up to the `LowLevelHooksTimeout` cap | every keystroke | Typing lag also outside copy/paste; input recovers in at most about 1 s |
  | e | **Heavy clipboard payload.** Large images or rich content processed by clipboard history (`cbdhsvc`), cloud sync or a remote-desktop bridge | the app copying | Correlates with copying images or large ranges |
- **Confidence:** high for a–c and e. For d, Windows' exact timeout and
  hook-removal behavior on 26200 is checked in the spike before the product
  depends on it.

### F-027 — The freeze can be measured from user mode without adding another hook
- **Evidence:** Documented APIs:
  - `RegisterRawInputDevices` with `RIDEV_INPUTSINK` delivers keyboard
    `WM_INPUT` to a background window. It is *not* part of the hook chain, so
    observing keystrokes does not add latency.
  - `AddClipboardFormatListener` gives a timestamped `WM_CLIPBOARDUPDATE`.
  - `GetClipboardOwner`, `GetOpenClipboardWindow`, `GetClipboardViewer`,
    `GetWindowThreadProcessId`, `IsHungAppWindow`, `SendMessageTimeout`.
- **Impact:** The tray process runs an **input watchdog** in the owner's
  session (a SYSTEM service in session 0 cannot see the user's clipboard):
  1. On a Ctrl+C keydown seen through raw input, it starts a timer and stops
     it at the next `WM_CLIPBOARDUPDATE`. That gives the **copy latency**.
  2. On Ctrl+V, it probes the foreground window with
     `SendMessageTimeout(WM_NULL, 100 ms)` every 100 ms for up to 5 s. That
     gives the **paste stall**.
  3. When latency is over 250 ms it records an **incident**: timestamps, the
     foreground app, the clipboard owner and its responsiveness, the window
     holding the clipboard open, the viewer-chain head, the clipboard
     formats and their size, the hung top-level windows, and which suspect
     processes (F-029) were running.
  4. Incidents go to SQLite and appear in a "Freezes" view grouped by
     mechanism (F-026) and by the process involved.
- **Confidence:** medium-high. The design is sound; the overhead and
  accuracy get validated in phase 1.

### F-028 — Hook owners cannot be listed through public APIs, so use elimination
- **Evidence:** Windows has no supported API that lists which processes own
  `WH_KEYBOARD_LL` hooks.
- **Impact:** Mechanism d is diagnosed by **guided elimination**. Once
  incidents have a baseline rate, MyClean offers to pause one suspect at a
  time, for example by quitting PowerToys Keyboard Manager for a day. Each
  pause needs the owner's approval and is restored automatically. MyClean
  then compares incident rates. For a single stubborn freeze, the docs explain
  how to capture a Windows Performance Recorder (WPR) trace for analysis in
  WPA.
- **Confidence:** medium.

### F-029 — Ranked suspects on this machine today
- **Evidence:** F-009 (running processes), and the owner's toolset
  (Substance, Blender, Unreal, browsers).

| Rank | Suspect | Likely mechanism | Why |
|---|---|---|---|
| 1 | `PowerToys.KeyboardManagerEngine` | d | Installs a low-level keyboard hook that inspects every key; remaps that involve Ctrl are on the exact path |
| 2 | AnyDesk (resident) | b, e | Remote-desktop tools synchronize the clipboard and may open it on every change |
| 3 | Clipboard history (`cbdhsvc`) | e | Copying large images from Substance/Blender makes the service serialize them |
| 4 | Armoury Crate, Discord | d | Both register global hotkeys and may use hooks |
| 5 | The source app itself (WebView2/Electron, Office, DCC tools) | a | Delayed rendering by a busy app stalls the paste in *another* app |

- **Impact:** A no-code test the owner can run right away: quit Keyboard
  Manager (or remove its Ctrl remaps) and close AnyDesk for a day, then note
  whether freezes continue. The MyClean watchdog replaces this guesswork
  with data from phase 1.
- **Confidence:** medium. These are ranked suspects, not a diagnosis.
