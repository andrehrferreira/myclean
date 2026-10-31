# 06 — The seven assistant prompts

The owner wants to test seven natural-language prompts against the machine.
This file explains how MyClean makes each one answerable and how to test it.

## Findings

### F-035 — Prompts become testable through an MCP server, not a chat UI
- **Evidence:** The prompts are written for an AI assistant that can inspect
  the computer. Claude Code / Claude Desktop already speak MCP (Model
  Context Protocol), and this repo already uses `.mcp.json`.
- **Impact:** `myclean mcp` is a stdio MCP server. It exposes MyClean's
  scanners as **read-only tools** plus **proposal tools** that create pending
  plans; those plans are approved in the tray (F-034). Testing a prompt then
  means registering the server and pasting the prompt as it is. The AI
  writes the explanation in the owner's language from structured data, so no
  chat UI and no API key are needed inside MyClean. The same functions back
  the CLI (`--json`) and the tray UI, so there is a single implementation.
- **Confidence:** high.

### F-036 — "Explain in simple terms" and "what I don't recognize" need a knowledge base and a trust score
- **Evidence:** Prompts 1, 2, 4 and 5.
- **Impact:** MyClean ships a curated, versioned **process & app knowledge
  base** (a JSON data file). Each entry matches on image name + signer + path
  pattern and holds a plain-language description, a category (system,
  driver/hardware, security, dev tool, updater, launcher, chat, game,
  OEM utility…), "needed at startup?" and "safe to remove?". Each process
  also gets a **trust signal**: Authenticode result (`WinVerifyTrust`,
  including catalog-signed system files), signer name, whether the image
  runs from a user-writable location (`%TEMP%`, `AppData`, `Downloads`), and
  whether the knowledge base knows it. "Unrecognized" means: not in the
  knowledge base **and** (unsigned, or an unknown signer, or a user-writable
  path). The AI may add context, but the flags come from these facts.
- **Confidence:** high.

### F-037 — "Not opened in six months" has no single source of truth
- **Evidence:** Windows records program use in several places, each with gaps:
  UserAssist (`HKCU\...\Explorer\UserAssist\{GUID}\Count`: names are ROT13,
  holds the last-run time, Explorer launches only); Prefetch
  (`C:\Windows\Prefetch\*.pf`: last 8 run times, Win10+ files are
  compressed with `XPRESS_HUFF` → `RtlDecompressBufferEx`, admin needed,
  capped at 1024 entries); BAM (`HKLM\SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\<SID>`,
  last-run times kept only for a short window).
- **Impact:** Prompt 4 combines all three and labels each answer with its
  source and confidence. From day 1 the service also logs every process
  start (F-022) to SQLite, so after MyClean has run for six months its own
  history becomes the authoritative answer. Installed apps come from the
  Uninstall keys (HKLM 64/32-bit, HKCU) and from Appx packages
  (`PackageManager.FindPackagesForUser`). "Came with the computer / keep" is
  true for: the OEM publisher (`Win32_ComputerSystem.Manufacturer` = ASUS),
  `SignatureKind::System` Appx, runtimes and frameworks (VC++ redists, .NET,
  DirectX, WebView2), and drivers.
- **Confidence:** high for the sources. Medium for the exact binary layouts,
  which get verified with fixtures in tests.

### F-038 — "Known to be dangerous" must come from a verdict, never a guess
- **Evidence:** Prompt 5 asks whether items are "known to be dangerous".
- **Impact:** Verdicts come only from: Microsoft Defender (detection history
  via WMI `root\Microsoft\Windows\Defender` `MSFT_MpThreatDetection`;
  on-demand file scan via `MpCmdRun.exe -Scan -ScanType 3 -File <path>`)
  and, opt-in, a **hash-only** VirusTotal lookup (the owner's API key; files
  are never uploaded). Everything else is reported as "suspicious because…"
  with the concrete reason.
- **Confidence:** high.

### F-039 — The monthly job must be scheduled inside MyClean, not with Task Scheduler
- **Evidence:** Prompt 7 says "configure this as a scheduled task". The
  project rule (AGENTS.md, rule 4) says: "No OS schedules (cron, systemd,
  launchd, schtasks): scheduling lives in the app."
- **Impact:** The service has its own scheduler: cron-like rules stored in
  SQLite, with **catch-up**. If the PC was off on day 1, the job runs at the
  next boot. "Configure as a scheduled task" therefore means "create a
  MyClean schedule". Interpretation stated here per the "state assumptions"
  rule.
- **Confidence:** high.

## Prompt-by-prompt

### 1 — Health check
> List every process above 5 % CPU or memory, explain each simply, flag what
> you don't recognize. Close nothing.

- **Data:** one `NtQuerySystemInformation` snapshot, plus a second one 1 s
  later for CPU deltas. CPU % is normalized to the whole machine (32 threads),
  the same way Task Manager shows it. Memory = private working set, as a % of
  127 GB. Processes are grouped into trees (Chrome with its 30 children counts
  as one entry, expandable).
- **Context the raw numbers miss:** also the top consumers over the **last
  24 h** from the governor's history, because "slow" is rarely one instant.
  Plus system signals: commit charge, C: free %, disk queue length.
- **MCP:** `health_snapshot`, `explain_process(pid)`. **CLI:**
  `myclean health`.
- **Accept when:** every qualifying process is listed with name, plain
  description (or "unknown"), signer, path and trust flag; the answer
  arrives in < 3 s; nothing is closed.

### 2 — Startup programs
> Show everything that starts automatically, say which I need, disable only
> what I select.

- **Sources:** `Run`/`RunOnce` (HKCU, HKLM, WOW6432Node); user and common
  Startup folders; the `StartupApproved` state (what Task Manager shows as
  enabled/disabled); scheduled tasks with logon/boot triggers (read through
  `ITaskService`); non-Microsoft auto-start services.
- **Impact column:** CPU and disk use per item during the first 2 minutes
  after logon, from MyClean's own ETW history. Until enough boots are
  recorded, `Diagnostics-Performance/Operational` events 100/101 are used
  where available.
- **Disable:** Run keys and Startup-folder items through `StartupApproved`
  (the same mechanism as Task Manager, so it is visible there and
  reversible); a task through `IRegisteredTask::put_Enabled(false)`; a service
  by setting its start type to Manual. Each one is a plan approved in the
  tray, with undo.
- **MCP:** `list_startup_items`, `propose_disable_startup(ids)`.

### 3 — Storage
> Top 20 files and folders, old downloads, duplicate photos, installers
> already used. Sizes. Delete nothing without permission.

- **Top 20:** from the scanner (F-017), per volume or path, as size on disk.
- **Old downloads:** files in `Downloads` older than 90 days, judged by
  creation/modification time. Last-access time is unreliable on NTFS. The
  `Zone.Identifier` stream shows where each file came from.
- **Duplicate photos:** exact duplicates by size → hash of the first 64 KB →
  full BLAKE3 hash. Near-duplicates (resized or re-encoded) by perceptual
  hash (WIC decode → 64-bit dHash, Hamming distance ≤ 6) in a later phase.
- **Installers already used:** `.msi` files whose `ProductCode` (read via
  `MsiOpenDatabase`) is installed (`MsiQueryProductState`); `.exe` files whose
  version info (`ProductName`/`CompanyName`) matches an Uninstall entry.
- **MCP:** `storage_top`, `find_old_downloads`, `find_duplicates`,
  `find_used_installers`, `propose_cleanup(items)`.

### 4 — Unused applications
> Apps not opened in six months, what each does, which are safe to uninstall,
> which came with the PC. I decide.

- **Data:** F-037. Each app shows: last used (with source and confidence),
  size, install date, publisher, knowledge-base description, and a verdict:
  *safe to remove* / *keep (OEM, runtime, driver)* / *unknown*.
- **Action:** a plan that launches the app's own uninstaller
  (`UninstallString`/`QuietUninstallString`, or `PackageManager.RemovePackageAsync`
  for Appx) after approval in the tray. MyClean never deletes program
  folders by hand.
- **MCP:** `list_installed_apps(unused_days=180)`, `propose_uninstall(app)`.

### 5 — Intruder scan
> Unrecognized processes, browser extensions I didn't install, programs
> talking to the internet in the background. Known dangerous? Remove nothing.

- **Processes:** trust signal (F-036).
- **Persistence points:** everything from prompt 2, plus WMI event
  subscriptions (`root\subscription`: `__EventFilter`/consumers), IFEO
  `Debugger` values, `Winlogon` `Shell`/`Userinit`, `AppInit_DLLs`.
- **Browser extensions** (Chrome, Edge, Brave, every profile; Firefox
  `extensions.json`): from each profile's `Preferences`/`Secure Preferences`,
  read the install time, `from_webstore` and the install `location`
  (store, external, policy, unpacked). Extensions forced by policy
  (`HKLM\SOFTWARE\Policies\<vendor>\<browser>\ExtensionInstallForcelist`) or
  installed externally are flagged as "probably not installed by you".
- **Background network:** `GetExtendedTcpTable`/`GetExtendedUdpTable`
  (owning PID) now, and, from the service, ETW
  `Microsoft-Windows-Kernel-Network` over time to catch periodic talkers
  that have no visible window. Remote addresses get a reverse-DNS name.
- **Verdicts:** F-038.
- **MCP:** `scan_suspicious`, `list_browser_extensions`,
  `list_network_processes`.

### 6 — Backups
> Is there any backup, where, and when was the last one? If none, explain the
> simplest setup with what I have.

- **Windows:** File History config
  (`%LOCALAPPDATA%\Microsoft\Windows\FileHistory\Configuration`); system
  images (`wbadmin get versions`, and `WindowsImageBackup` folders);
  restore points (WMI `root\default:SystemRestore`); shadow copies
  (`Win32_ShadowCopy`); OneDrive folder backup (registry under
  `HKCU\Software\Microsoft\OneDrive\Accounts`; exact values to be verified).
- **Third-party tools:** installed products (Macrium, Veeam Agent, Acronis,
  Backblaze, Duplicati…), plus a shallow scan of K: and L: for backup
  artifacts (`.mrimg`, `.vbk`, `.tibx`, `WindowsImageBackup`, File History
  folders), reporting the newest timestamp.
- **Developer angle:** git repositories under the workspace roots with no
  remote, unpushed commits or uncommitted work. That code exists only on
  this disk.
- **No backup found:** a short, concrete recommendation using the owner's
  own hardware (for example File History → K:, and a monthly system image →
  L:). Explained only; MyClean does not configure it.
- **MCP:** `check_backups`.

### 7 — Monthly cleanup and report
> On the 1st of each month: health check, storage analysis, intruder scan,
> then a short report of what changed. Make it a scheduled task.

- **Schedule:** the in-app scheduler (F-039), "1st of month 09:00,
  catch-up on".
- **Run:** prompts 1, 3 and 5. The health part uses **30-day aggregates**
  from the governor history, not just one sample.
- **Diff against last month's snapshot:** new startup items, extensions,
  services, listening ports and unsigned binaries; growth of the top
  folders; C: free-space trend; governor interventions; Ctrl+C/V freeze
  incidents; and space freed by `auto` cleaning.
- **Delivery:** Markdown + HTML report in `%ProgramData%\MyClean\reports\YYYY-MM\`
  and a toast with an "Open report" link. Email over SMTP (credentials in
  Windows Credential Manager) is optional and comes later.
- **MCP:** `get_report(month)`, `compare_snapshots(a, b)`.

## How to test the prompts

1. Build MyClean and register the server in this repo's `.mcp.json`
   (`"myclean": {"command": "<path>\\myclean.exe", "args": ["mcp"]}`).
2. In Claude Code, paste a prompt as it is.
3. A prompt passes when the answer covers every data source listed for it,
   explains each item, and executed nothing (the audit table must show no
   action without a tray approval). Each prompt also gets an automated test
   that calls the MCP tools against a fixture machine state.
