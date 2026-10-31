# Security Specification

## ADDED Requirements

### Requirement: Persistence Point Inventory
The system SHALL enumerate every persistence point on the machine: `Run`/`RunOnce` keys (HKCU, HKLM, WOW6432Node), user and common Startup folders, scheduled tasks with logon or boot triggers (read through `ITaskService`), auto-start services, WMI event subscriptions (`root\subscription` `__EventFilter` and consumers), IFEO `Debugger` values, `Winlogon` `Shell` and `Userinit`, and `AppInit_DLLs`. Each entry MUST carry its source location, the target image path, the signer, the trust signal (SEC-1) and the knowledge-base match, if any.

#### Scenario: All persistence sources are reported
Given a machine with one HKCU Run value, one logon-triggered task, one WMI `__EventFilter` bound to a `CommandLineEventConsumer`, and one IFEO `Debugger` value on `notepad.exe`
When the owner runs the intruder scan
Then the result lists all four entries, each with its source location, target image path, signer and trust signal

#### Scenario: Default Winlogon values are not flagged
Given `Winlogon` `Shell` is `explorer.exe`, `Userinit` is the Windows default `userinit.exe` path, and `AppInit_DLLs` is empty
When the owner runs the intruder scan
Then the Winlogon and AppInit entries are reported as default and are not flagged as suspicious

### Requirement: Browser Extension Inventory With Install Origin
The system SHALL list the extensions of every Chromium-based browser profile (Chrome, Edge, Brave; every profile, read from `Preferences` and `Secure Preferences`) and of every Firefox profile (`extensions.json`), with name, ID, browser, profile, install time and install origin (store, external, policy, unpacked). An extension forced by policy (`HKLM\SOFTWARE\Policies\<vendor>\<browser>\ExtensionInstallForcelist`) or installed externally MUST be flagged "probably not installed by you".

#### Scenario: Policy-forced extension is flagged
Given an Edge profile contains an extension whose ID appears in `HKLM\SOFTWARE\Policies\Microsoft\Edge\ExtensionInstallForcelist`
When `list_browser_extensions` is called
Then that extension is returned with origin `policy` and the flag "probably not installed by you"

#### Scenario: Store extension in a second profile is listed
Given Chrome has two profiles and the second one holds an extension installed from the Chrome Web Store (`from_webstore` true)
When `list_browser_extensions` is called
Then the extension is listed under the second profile with origin `store` and without the "probably not installed by you" flag

#### Scenario: Externally installed extension is flagged
Given a Brave profile contains an extension whose install location is external (registry or external file)
When `list_browser_extensions` is called
Then that extension is returned with origin `external` and the flag "probably not installed by you"

### Requirement: Background Network Talkers Over Time
The system SHALL report processes that use the network, combining the current TCP/UDP tables with owning PID (`GetExtendedTcpTable`/`GetExtendedUdpTable`) and the service's ETW `Microsoft-Windows-Kernel-Network` history over a requested time window. Each entry MUST show the process, signer, trust signal, remote addresses, bytes sent and received, first and last seen time, and whether the process owned a visible window. Reverse-DNS names SHALL be resolved only when the owner has enabled the reverse-DNS setting, which is off by default, because a lookup sends the remote address to the DNS resolver (NFR-SEC-3).

#### Scenario: Periodic talker without a window is caught
Given a windowless process connected to the same remote address every 15 minutes during the last 24 hours but has no open connection right now
When `list_network_processes` is called for the last 24 hours
Then the process is listed from the ETW history with its remote address, byte counts, first and last seen times and "no visible window"

#### Scenario: Reverse DNS off by default
Given the reverse-DNS setting has never been changed
When `list_network_processes` is called
Then every remote address is shown as a raw IP with no name and MyClean performs no DNS query

#### Scenario: Reverse DNS enabled by the owner
Given the owner enabled the reverse-DNS setting
When `list_network_processes` is called
Then each remote address that resolves is shown with its reverse-DNS name

#### Scenario: Unresolvable address is still listed
Given the reverse-DNS setting is enabled and a process holds a TCP connection to an address that has no reverse-DNS record
When `list_network_processes` is called
Then the connection is listed with the raw IP address and an empty name, and the scan does not fail

### Requirement: Verdicts Only From Defender Or Opt-In VirusTotal
The system SHALL label an item "known dangerous" only when Microsoft Defender reports a detection for it (detection history from WMI `root\Microsoft\Windows\Defender` `MSFT_MpThreatDetection`, or an on-demand `MpCmdRun.exe -Scan -ScanType 3 -File <path>` scan) or when the opt-in VirusTotal hash lookup reports it as malicious. Every other finding MUST be reported as "suspicious because <reason>" with the concrete reason (for example unsigned, unknown signer, runs from a user-writable path, not in the knowledge base, non-default Winlogon value), and never as dangerous.

#### Scenario: Defender detection produces a verdict
Given `MSFT_MpThreatDetection` holds a detection whose resource path matches a Run-key target
When `scan_suspicious` is called
Then that Run-key entry is labeled "known dangerous" with source "Microsoft Defender" and the threat name

#### Scenario: Unsigned binary without verdict is only suspicious
Given an unsigned binary in `%LOCALAPPDATA%\Temp` is a Run-key target, is not in the knowledge base, and neither Defender nor VirusTotal reports it
When `scan_suspicious` is called
Then the entry is reported as "suspicious because unsigned, runs from a user-writable path, not in the knowledge base" and is not labeled dangerous

### Requirement: On-Demand Defender Scan Removes Nothing
The system SHALL run on-demand Defender file scans only on the file of a reported item and MUST pass `-DisableRemediation`, so that the scan MyClean starts reports a result without quarantining or deleting the file.

#### Scenario: Scan of a suspicious file reports only
Given a suspicious persistence target exists on disk
When the owner asks MyClean to scan that file with Defender
Then MyClean runs `MpCmdRun.exe -Scan -ScanType 3 -File <path> -DisableRemediation`, records the result as the item's verdict, and the file is still present afterwards

### Requirement: VirusTotal Lookup Is Opt-In And Hash-Only
The system SHALL keep the VirusTotal lookup disabled by default and MUST allow enabling it only after the owner provides an API key. When enabled, the system MUST send only the file's SHA-256 hash, never the file, its name, its path or any other machine data. The result MUST be shown as the number of engines that flag the hash as malicious out of the engines that answered.

#### Scenario: VirusTotal disabled makes no network call
Given VirusTotal is disabled (the default)
When `scan_suspicious` runs over 200 items
Then no network request is made to VirusTotal or to any other host for verdicts, and every verdict comes from Defender or is absent

#### Scenario: No API key keeps VirusTotal off
Given the owner has not stored a VirusTotal API key
When the owner tries to enable the VirusTotal lookup
Then the setting stays off and the UI explains that an API key is required

#### Scenario: Enabled lookup sends only the hash
Given VirusTotal is enabled with the owner's API key
When an unsigned binary is checked
Then the only data sent is its SHA-256 hash, the file is not uploaded, and the result shows "flagged by k of n engines" with source "VirusTotal"

### Requirement: Intruder Scan Changes Nothing
The system SHALL treat the intruder scan and its MCP tools (`scan_suspicious`, `list_browser_extensions`, `list_network_processes`) as read-only. No process MUST be closed, and no file, extension, persistence point or service MUST be removed or disabled as a result of a scan. Disabling a persistence point MUST go through a plan approved in the tray.

#### Scenario: Scan leaves the audit log without actions
Given the intruder scan reports three suspicious items and one Defender detection
When the scan completes
Then the `action_log` contains no action executed as a result of the scan, and every reported process is still running

#### Scenario: MCP client cannot disable an item
Given an MCP client has called `scan_suspicious` and received a suspicious Run-key entry
When the client asks MyClean to disable that entry
Then MyClean can at most create a plan in `awaiting_approval`, and the entry stays enabled until the owner approves the plan in the tray

### Requirement: Prompt 5 Answerable Through MCP
The system SHALL expose `scan_suspicious`, `list_browser_extensions` and `list_network_processes` as MCP read tools, with the same data available from the CLI with `--json`, so that prompt 5 (intruder scan) can be answered from structured data with every item explained and "dangerous" used only for verdicts.

#### Scenario: Prompt 5 against fixture state
Given a fixture machine state with one unrecognized process, one policy-forced extension, one windowless network talker and one Defender detection
When prompt 5 is pasted into Claude Code with `myclean mcp` registered
Then the answer covers all four items with a plain explanation, only the Defender detection is called dangerous, and the audit log shows no action
