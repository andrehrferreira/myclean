# Apps Specification

## ADDED Requirements

### Requirement: Installed App Inventory
The system SHALL list every installed app from the Uninstall keys (HKLM 64-bit, HKLM 32-bit `WOW6432Node`, HKCU) and from Appx packages (`PackageManager.FindPackagesForUser`), with name, publisher, version, install date, size, and the knowledge-base description when the knowledge base knows the app. Apps without a knowledge-base entry MUST show the description "unknown" rather than an invented one.

#### Scenario: Win32 and Appx apps appear together
Given one app registered under HKLM 32-bit Uninstall, one under HKCU Uninstall and one Appx package for the current user
When `list_installed_apps` is called
Then all three apps are returned with name, publisher, version, install date and size

#### Scenario: App missing from the knowledge base
Given an installed app whose name, signer and path match no knowledge-base entry
When `list_installed_apps` is called
Then the app is returned with the description "unknown"

### Requirement: Multi-Source Last-Used With Source And Confidence
The system SHALL determine each app's last-used time by fusing UserAssist (`HKCU\...\Explorer\UserAssist\{GUID}\Count`, ROT13 names), Prefetch (`C:\Windows\Prefetch\*.pf`, including Win10+ `XPRESS_HUFF`-compressed files), BAM (`HKLM\SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\<SID>`) and MyClean's own process-start history. Every last-used answer MUST be labeled with the source it came from and a confidence level, and an app with no evidence from any source MUST be reported as "no usage recorded" with its confidence, never as a fabricated date.

#### Scenario: Newest timestamp wins and names its source
Given UserAssist records an app's last run on 2026-03-02 and Prefetch records 2026-05-10 for the same executable
When the app's last-used time is computed
Then the answer is 2026-05-10 with source "Prefetch" and a confidence level

#### Scenario: Own history becomes authoritative
Given MyClean's process-start history covers more than 180 days and records the app's last start on 2026-09-20
When the app's last-used time is computed
Then the answer is 2026-09-20 with source "MyClean process history" and high confidence

#### Scenario: No evidence from any source
Given no source holds any run record for an app and MyClean's history covers only 40 days
When the app's last-used time is computed
Then the app is reported as "no usage recorded" with low confidence and the 40-day history span is stated

### Requirement: Unused App Filter
The system SHALL treat an app as unused when it has not been used in 180 days, and `list_installed_apps(unused_days=180)` MUST return those apps together with apps that have no usage recorded, each with its last-used source and confidence.

#### Scenario: Filter keeps only unused apps
Given app A was last used 200 days ago, app B 30 days ago, and app C has no usage recorded
When `list_installed_apps` is called with `unused_days=180`
Then A and C are returned with their last-used source and confidence, and B is not returned

### Requirement: Keep Or Remove Verdict
The system SHALL classify each app as `removable` (safe to remove), `keep` or `unknown`, and MUST attach the reason. The verdict MUST be `keep` for apps from the OEM publisher (the manufacturer from `Win32_ComputerSystem.Manufacturer`, read at runtime), Appx packages with `SignatureKind::System`, runtimes and frameworks (VC++ redistributables, .NET, DirectX, WebView2) and drivers. `removable` MUST only come from a knowledge-base entry that marks the app safe to remove; every other app is `unknown`.

#### Scenario: OEM app is kept
Given `Win32_ComputerSystem.Manufacturer` is "ASUSTeK COMPUTER INC." and an installed app's publisher is ASUS
When apps are classified
Then that app's verdict is `keep` with reason "came with the computer (OEM)"

#### Scenario: Runtime is kept
Given the Microsoft Visual C++ 2015-2022 Redistributable is installed
When apps are classified
Then its verdict is `keep` with reason "runtime or framework"

#### Scenario: System Appx is kept
Given an Appx package whose `SignatureKind` is `System`
When apps are classified
Then its verdict is `keep` with reason "Windows system app"

#### Scenario: Unknown app is not guessed removable
Given an app with no knowledge-base entry that is not OEM, runtime, driver or system Appx
When apps are classified
Then its verdict is `unknown`

### Requirement: Uninstall Through The App's Own Uninstaller After Approval
The system SHALL uninstall an app only by launching the app's own uninstaller (the registered `UninstallString` or `QuietUninstallString`, or `PackageManager.RemovePackageAsync` for Appx), as a `LaunchUninstaller` plan that runs only after the owner approves it in the tray. The system MUST never delete program folders or registry entries of an app by hand, and the plan preview MUST state that the action is not reversible except by reinstalling.

#### Scenario: Approved plan launches the uninstaller
Given a plan to uninstall app A has been approved in the tray
When the plan executes
Then MyClean launches A's registered uninstaller, records the action in `action_log` with proposer and approver, and afterwards re-reads the Uninstall keys to record whether A is gone

#### Scenario: Nothing is uninstalled without approval
Given a plan to uninstall app A is in `awaiting_approval`
When the owner has not approved it
Then no uninstaller is launched and A remains installed

#### Scenario: Program folder is never deleted by hand
Given app A's uninstaller finishes but leaves its folder under `C:\Program Files\A`
When the plan completes
Then MyClean reports the leftover folder and does not delete it

#### Scenario: App without an uninstaller
Given a Win32 app whose Uninstall key has no `UninstallString` and no `QuietUninstallString`
When an uninstall is proposed for it
Then no plan is created and the reason "no uninstaller registered" is returned

### Requirement: MCP Proposals Cannot Approve
The system SHALL let an MCP client call `propose_uninstall(app)` to create a plan in `awaiting_approval`, which triggers a tray notification, and MUST never let an `mcp:*` actor move the plan to `approved`. A proposal for an app with a `keep` verdict MUST carry the keep reason in the plan shown in the tray.

#### Scenario: MCP proposal waits for the tray
Given an MCP client calls `propose_uninstall` for an unused app
When the call returns
Then a plan exists in `awaiting_approval` with actor `mcp:<client>`, a tray notification is shown, and the app is still installed

#### Scenario: Keep reason travels with the proposal
Given an MCP client proposes uninstalling a VC++ redistributable
When the owner opens the plan in the tray
Then the plan shows the verdict `keep` and the reason "runtime or framework"

### Requirement: Prompt 4 Answerable Through MCP
The system SHALL expose `list_installed_apps` and `propose_uninstall` as MCP tools, with the read data also available from the CLI with `--json`, so that prompt 4 (unused applications) can be answered with last-used and its source, description and verdict for every app.

#### Scenario: Prompt 4 against fixture state
Given a fixture machine state with an unused third-party app, an unused OEM utility, a runtime and an app with no usage recorded
When prompt 4 is pasted into Claude Code with `myclean mcp` registered
Then every app in the answer has last-used with source, a description (or "unknown") and a verdict, and the audit log shows no uninstall without a tray approval
