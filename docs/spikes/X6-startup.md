# X6 — Startup items

- **Work package:** O1 (phase 4, `.rulebook/tasks/phase4_optimize`)
- **Prompt:** 2, startup programs (`docs/analysis/myclean-planning/06-assistant-prompts.md`)
- **Spec:** `.rulebook/tasks/phase4_optimize/specs/startup/spec.md`, requirements
  **Startup Inventory**, **Measured Startup Impact**, **Disable and Enable
  Through Native Mechanisms**, **Exact Startup Undo**, **Boot and Logon Time
  Tracking**
- **Machine:** Windows 11 Pro build 26200, pt-BR, run 2026-10-31,
  **non-elevated** (Administrators SID deny-only, UAC on, ConsentPromptBehaviorAdmin=5).
- **System state:** read-only. The only write was a throwaway
  `StartupApproved\Run\MyCleanSpikeX6` value in HKCU, removed afterwards (see §3).

## Questions

1. Where are all the startup sources, and what does each look like on this machine?
2. What is the exact `StartupApproved` byte layout, and what do the first-byte
   codes mean?
3. Does the disable/enable round trip work, and does it match Task Manager?
4. What can a non-elevated process read: scheduled tasks via ITaskService,
   services, and Diagnostics-Performance events 100/101?

## 1. Run / RunOnce keys and Startup folders

### Method

```powershell
# Explicit registry views. NOTE: the agent's pwsh was 32-bit (C:\Program Files (x86)\PowerShell\7),
# so HKLM:\Software\... in the provider silently showed the WOW6432Node view.
$b64=[Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine','Registry64')
$b32=[Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine','Registry32')
$b64.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run').GetValueNames()
$b32.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run').GetValueNames()   # = WOW6432Node
[Environment]::GetFolderPath('Startup'); [Environment]::GetFolderPath('CommonStartup')
```

### Observed

| Source | Items |
|---|---|
| HKCU `…\Run` | 11 (OneDrive, Discord, Steam, EpicGamesLauncher, Edge/Chrome auto-launch, OpenVPN, Lists, Teams, Acrobat Synchronizer, Docker Desktop) |
| HKCU `…\RunOnce` | 5 (OneDrive updater leftovers such as `Uninstall 26.168.0830.0006`) |
| HKLM `…\Run` (64-bit view) | 7 (SecurityHealth, RtkAudUService, AdobeAAMUpdater, Autodesk Access ×2, Elgato Control Center, PenTablet) |
| HKLM `…\Run` (32-bit view = `WOW6432Node`) | 3 (Adobe CCXProcess, Adobe Creative Cloud, `Autodesk Genuine Service ` **with a trailing space**) |
| HKLM `…\RunOnce` (32-bit) | 1 (`InnoSetupRegFile.0000000001`) |
| HKCU WOW6432Node Run, Policies\Explorer\Run (HKCU/HKLM) | absent |
| User Startup folder | `Reallusion Hub.lnk`, `cortex-consolidator-daemon.vbs` (**not a .lnk**), `desktop.ini` (hidden) |
| Common Startup folder | 6 items, including `qt_temp.p11040` (a leftover temp file, **not a .lnk**), plus `desktop.ini` |

- A 32-bit process reading `HKLM\Software\...\Run` gets the WOW6432Node
  values. Reading `HKLM\Software\WOW6432Node\...\Run` explicitly in the 64-bit
  view gives the same 3 values. Enumerating both paths **and** both views
  double-counts.
- In the 32-bit, non-elevated pwsh, `OpenSubKey(HKLM\…\Run, writable:true)`
  **succeeded** because of **UAC registry virtualization** (writes would go to
  `VirtualStore`). A native x64 probe with `RegOpenKeyExW(KEY_SET_VALUE|KEY_WOW64_64KEY)`
  got **error 5** on HKLM Run and on all HKLM `StartupApproved` keys. The ACLs
  are Administrators/SYSTEM FullControl only.

## 2. StartupApproved format

### Method

```powershell
$sa='Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved'
foreach ($s in 'Run','Run32','StartupFolder') { foreach ($hive in 'CurrentUser','LocalMachine') {
  $k=[Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,'Registry64').OpenSubKey("$sa\$s")
  foreach ($n in $k.GetValueNames()) { $v=$k.GetValue($n)       # REG_BINARY, 12 bytes
    $ft=[BitConverter]::ToInt64($v,4); "$n : $($v[0]) : $([DateTime]::FromFileTimeUtc($ft))" } } }
```

### Observed

- Subkeys: HKCU has `Run` and `StartupFolder`, and **no `Run32`**. HKLM has
  `Run`, `Run32` and `StartupFolder`. There is **no** StartupApproved key for
  RunOnce.
- Mapping: `StartupApproved\Run` ↔ the 64-bit `Run` key of the same hive,
  `Run32` ↔ `WOW6432Node\…\Run`, `StartupFolder` ↔ Startup folder **file
  names** (including the `.lnk` extension, and non-`.lnk` files such as
  `cortex-consolidator-daemon.vbs` and `qt_temp.p11040`).
- Every value is **REG_BINARY, 12 bytes**: a 4-byte little-endian DWORD of
  flags, then an **8-byte FILETIME (UTC)** at offset 4.
- First bytes seen (bytes 1–3 always `00 00 00`):

| Byte 0 | Count | FILETIME | Running-process check |
|---|---|---|---|
| `02` | 17 | always zero | Docker Desktop, Teams (02) are running |
| `03` | 33 | usually the time it was disabled; **zero in 6 cases** (for example `com.squirrel.Postman-Agent`, 5 HKCU StartupFolder values) | OneDrive, EpicGamesLauncher, Control Center, PenTablet, Autodesk Access (03) are not running. **Discord and Steam (03) are running**, started 3 min and 3 days after logon, so possibly by hand. |
| `07` | 3 (all HKLM: `SecurityHealth`, `StartRLCMS.lnk`, `UMC Control Panel Autostart.lnk`) | non-zero | SecurityHealthSystray (07) is not running |
| `06` | not seen | — | — |

  Reading: bit 0 set (`03`, `07`) = disabled, `02` = enabled. The meaning of
  bit 2 (`07` vs `03`) is **not determined**; treat `07` as disabled.
- **Orphans:** values without a matching Run entry or file. HKCU Run: 11 of
  21 (for example `Camera Hub`, `NordVPN`, `RiotClient`, `Overwolf`, and
  `EPLTarget\P0000000000000000`, whose **value name contains a backslash**).
  HKLM Run: 5 of 12. HKLM Run32: 1 of 4 (`RadminVPN`). HKCU StartupFolder:
  `Ollama.lnk`. Windows leaves them behind when an app removes its Run value.
- **Missing values:** HKCU Run `org.openvpn.client` has no StartupApproved
  value. It is treated as enabled.
- **HKCU StartupFolder also lists files from the common Startup folder**
  (`AnyDesk.lnk`, `GoXLR App.lnk`, `MxNotify.lnk` …). For `GoXLR App.lnk`,
  HKCU says `03` (zero time) while HKLM says `02`, and `GoXLR App` is running
  (started about 5 min after logon). Which hive controls common-folder items
  is **inconclusive**.

## 3. Round trip on a throwaway value

### Method

Only the StartupApproved value was written, with **no Run value**. That way
nothing could launch, and no "new startup app" notification could be
triggered. Windows' `Windows.SystemToast.StartupApp` notification is
`Enabled=0` here anyway.

```powershell
$b=[Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser','Registry64')
$sa=$b.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run',$true)
$sa.SetValue('MyCleanSpikeX6',[byte[]](@(3,0,0,0)+[BitConverter]::GetBytes([DateTime]::UtcNow.ToFileTimeUtc())),'Binary')
$sa.SetValue('MyCleanSpikeX6',[byte[]](2,0,0,0,0,0,0,0,0,0,0,0),'Binary')
$sa.DeleteValue('MyCleanSpikeX6')
```

### Observed

- Disable wrote `03 00 00 00 6C A5 B6 B6 FE 68 DD 01` (FILETIME
  2026-10-31T06:11:48Z). Enable wrote `02 00…00`. The values read back exactly,
  as REG_BINARY. Non-elevated HKCU writes work.
- Cleanup was verified: the value is absent afterwards, and there is no Run value
  `MyCleanSpikeX6`.
- **Task Manager / Settings › Startup apps view: not verified.** Opening UI is
  out of bounds for this unattended run, and Explorer's honoring of the value
  can only be observed at the next logon. Preliminary evidence from running
  processes is consistent with "03/07 = not started at logon" for most items,
  but Discord/Steam (03, running) make it inconclusive.

## 4. Scheduled tasks (ITaskService)

### Method

```powershell
$ts=New-Object -ComObject Schedule.Service; $ts.Connect()          # ITaskService
function Walk($f){ foreach($t in $f.GetTasks(1)){ $t } ; foreach($s in $f.GetFolders(0)){ Walk $s } }  # 1 = TASK_ENUM_HIDDEN
Walk ($ts.GetFolder('\')) | % { $_.Definition.Triggers | % Type }  # 8 = BOOT, 9 = LOGON
```

### Observed

- Non-elevated: **210 tasks** enumerated, with **0** folder errors and **0**
  unreadable definitions. With `TASK_ENUM_HIDDEN` the root folder shows 14
  tasks instead of 11.
- **51** tasks have a logon (9) or boot (8) trigger. 41 are under
  `\Microsoft\`; the other 10 are Adobe ×2, `OneDrive Startup Task-<SID>`,
  ASUS ×6 and `PowerToys\Autorun for <user>`. **Microsoft-made tasks also live
  outside `\Microsoft\`** (OneDrive), so the folder path is not a publisher test.
- We cannot check whether tasks with restrictive security descriptors are
  silently left out: `C:\Windows\System32\Tasks` is not listable non-elevated.
  The service, running as SYSTEM, should do the inventory.

## 5. Auto-start services

### Method

```powershell
Get-CimInstance Win32_Service | ? StartMode -eq 'Auto'
# binary = ImagePath, or Parameters\ServiceDll for svchost services; DelayedAutostart and TriggerInfo from the service key
Get-AuthenticodeSignature -LiteralPath $binary    # SignerCertificate.Subject CN
```

### Observed

- **113** auto-start services. Signers: `Microsoft Windows` 68,
  `Microsoft Corporation` 3 (`edgeupdate`, `WSLService`, `IpOverUsbSvc`),
  **`Microsoft Windows Hardware Compatibility Publisher` 7**, third-party 32,
  and `NotSigned` 3 (`HoudiniLicenseServer`, `MongoDB`, `postgresql-x64-18`).
- The 7 WHQL-signed services are **third-party**: AMD ×4, Realtek, Nahimic and
  NVIDIA Display Container. The signer subject says Microsoft, but
  `VersionInfo.CompanyName` names the vendor.
- Among the 45 non-`Microsoft Windows` services, `DelayedAutostart=1` is set
  on 4 (`asus`, `edgeupdate`, `gupdate`, `KSDE5.26`), and `TriggerInfo` exists
  on `edgeupdate`. Undo must keep both.
- `HKLM\SYSTEM\CurrentControlSet\Services\nsi\Parameters` cannot be read
  non-elevated (`Requested registry access is not allowed`).

## 6. Diagnostics-Performance events 100/101

### Method

```powershell
Get-WinEvent -ListLog 'Microsoft-Windows-Diagnostics-Performance/Operational'
Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Diagnostics-Performance/Operational'; Id=100,101} -MaxEvents 5
wevtutil gl Microsoft-Windows-Diagnostics-Performance/Operational
```

### Observed

- `-ListLog` gives "start PowerShell with elevated user rights … unauthorized
  operation". The event query returns **"No events were found"**, which is how
  access denied shows up, not a real empty result.
- Channel SDDL: `O:BAG:SYD:(D;;0xf0007;;;AN)(D;;0xf0007;;;BG)(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x5;;;LS)(A;;0x5;;;NS)(A;;0x1;;;SU)`.
  SYSTEM, elevated Administrators, LocalService, NetworkService and service
  logons can read. **Interactive users cannot.** The log is enabled, and the DPS
  service is Running/Automatic. *Whether events 100/101 exist on this machine
  is therefore not verified.*
- Readable non-elevated: `System` (Kernel-General 12 = OS start, last on
  2026-09-30 09:01; Kernel-Boot 27 present) and
  `Microsoft-Windows-Winlogon/Operational`. Interactive logon session start
  (Win32_LogonSession type 2): 2026-09-30 09:24:54.

## Conclusions

- The inventory sources in the spec are right. Enumeration must use explicit
  64- and 32-bit registry views (`KEY_WOW64_64KEY`/`KEY_WOW64_32KEY`) with no
  double counting, keep value names byte-exact (trailing spaces, backslashes),
  and include non-`.lnk` Startup-folder files while skipping `desktop.ini`.
- StartupApproved values are 12 bytes: `02` + zero FILETIME = enabled, `03` +
  the disable FILETIME = disabled. `07` is also found and must be read as
  disabled. A missing value means enabled. Orphan values are common and are
  not startup items.
- Disabling an HKLM Run/Run32/common-Startup item needs admin, because the
  HKLM StartupApproved keys deny write to non-elevated users. So the **service**
  must perform those changes, not the tray. MyClean binaries must be x64 and
  carry a `requestedExecutionLevel` manifest, so registry virtualization can
  never silently redirect a write.
- Events 100/101 are readable only by the service (SYSTEM), not by a
  non-elevated CLI.

## Implications for the specs

1. **Startup Inventory** — add: read the Run keys through both registry views
   explicitly; list `RunOnce` items without an enabled state (StartupApproved
   doesn't cover RunOnce); include non-`.lnk` Startup-folder files; ignore orphan
   StartupApproved values (optionally report them as "leftovers").
2. **Startup Inventory, "Disabled in Task Manager"** — define the state: byte 0
   `02` = enabled; `03` or `07` (bit 0 set) = disabled; no value = enabled.
   Matching Task Manager's view is **not verified** and needs one manual check
   by the owner (disable an item in Task Manager, then read the value).
3. **Startup Inventory, "Microsoft services excluded"** — signer subject
   `Microsoft Windows` is a correct exclusion test. But `Microsoft Windows
   Hardware Compatibility Publisher` (WHQL) must count as **third-party**
   (AMD, Realtek, NVIDIA, Nahimic here). Unsigned services (MongoDB, PostgreSQL)
   must be listed. Task publisher must come from the action binary's signer,
   not from the `\Microsoft\` folder path.
4. **Disable and Enable Through Native Mechanisms** — HKLM items (Run,
   Run32, common Startup folder) need an elevated writer. Route them through
   the service; HKCU items can be written by either. To match Task Manager,
   disable writes `03 00 00 00` + current FILETIME (UTC); enable writes
   `02 00 00 00` + 8 zero bytes.
5. **Exact Startup Undo** — store the **full 12 bytes**, or "value absent", as
   the previous state. Many existing values are `03` with a zero time or `07`,
   and Explorer's handling of those differences is unknown.
6. **Measured Startup Impact, "Fallback before history exists"** — events
   100/101 need SYSTEM/admin. The service can read them; the CLI/MCP must get
   them through the service. If they are absent, use Kernel-General 12 and the
   Winlogon/Operational timestamps, which are readable non-elevated, for
   boot/logon times (**Boot and Logon Time Tracking**).
7. Open item for O1: on the next interactive session, the owner checks that
   Task Manager shows a `03`/`07` item as disabled and which hive governs a
   common-Startup item (`GoXLR App.lnk`: HKCU 03 vs HKLM 02).
