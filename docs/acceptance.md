# Seven-prompt acceptance

The owner's seven prompts ([prd.md](prd.md) §7) are MyClean's acceptance test. Each one has an
automated test that runs in the normal suite and a manual pass before every release: the prompt
pasted as written in Claude Code (through `myclean mcp`) and asked in the MyClean UI.
Requirement: "Seven-Prompt Acceptance Suite" in
`.rulebook/tasks/phase6_monthly-report-v1/specs/reports/spec.md`. The prompts and how MyClean
answers each come from
[analysis/myclean-planning/06-assistant-prompts.md](analysis/myclean-planning/06-assistant-prompts.md).

## The automated suite

`tests/acceptance/` (ctest label `acceptance`) asks each prompt with the MCP tools a client calls
for it, through `mcp::Server::handle` exactly as Claude Code's JSON-RPC requests arrive, and
checks the prompt's pass condition. It runs with every `ctest` and so in the pre-commit gate.

```powershell
ctest --preset debug -L acceptance                  # the whole suite
ctest --preset debug -R "AcceptanceTest.Prompt3"    # one prompt
```

### The fixture machine

`tests/acceptance/acceptance_fixture.hpp` builds one machine from the fakes of the unit tests
(`tests/fakes`, the `*_fakes.hpp` headers, `tests/unit/svc/phase1_fakes.hpp`,
`tests/unit/cleaning/cleaning_fixture.hpp`). As in `myclean-svc`, the components share one store
(every component's tables, `action_log` included) and one dispatcher: trust (the shipped
knowledge packs), telemetry (its pipeline polled by the test), security, cleaning (space, startup,
apps, plans) and backup. The MCP server reaches that dispatcher as any client but the tray does;
the tests make the owner's clicks (approve, undo) as the tray caller. Nothing touches the real
PC: no file, registry value, process, Defender scan or network call.

| Area | What the machine holds (2026-10-31 12:00 UTC) |
|---|---|
| Processes | PowerToys (signed by Microsoft, in the knowledge base, 9 % CPU), `editor.exe` (unsigned, in the owner's AppData, 12.5 % of memory), `small.exe` (below 5 %, image unreadable) |
| Startup | Run values Discord, Steam, Updater (unsigned, in `%TEMP%`), Agent (a Defender detection); Startup-folder `Spotify.lnk`; logon task OneDrive; auto service ArmouryCrateService; one logon measured by MyClean |
| Files | 25 large files in `C:\Data`; Downloads with a 300-day-old download, an `.msi` and an `.exe` of installed programs; Pictures with a resized copy and an exact duplicate |
| Apps | Old Tool (Prefetch, 200 days, safe to remove), Board Utility (the maker's, UserAssist, 400 days), the Visual C++ runtime (no usage), Game Launcher (BAM, 5 days) |
| Security | Edge: a store extension and a policy-forced one; Chrome: one a program installed; PowerToys and `editor.exe` connected, `editor.exe` listening on 8080 and sending every hour for 6 hours |
| Backups | Restore points only; K: "Backup 1" and L: "Backup 2" healthy HDDs; repositories `F:\app` (no remote), `F:\lib` (unpushed and uncommitted), `F:\ok` |

### What each prompt test checks

| # | Tools | Pass condition checked |
|---|---|---|
| 1 | `health_snapshot`, `explain_process`, `top_processes` | Exactly the trees above 5 % CPU or memory (PowerToys, `editor.exe`) are listed, each with its trust signal: signer and the knowledge-base description, or flagged unrecognized with its reasons and a null description (never a guess); `explain_process` of each agrees; no "dangerous" or "malicious" anywhere; every process still runs |
| 2 | `list_startup`, `propose_disable_startup` | All four sources (Run, Startup folder, task, service) with MyClean's measured impact; the proposal waits for the tray and changes nothing; the owner approves two of the three proposed items and only those two are disabled; undo restores them |
| 3 | `storage_top`, `find_old_downloads`, `find_duplicates` (photos and exact), `find_used_installers`, `propose_cleanup` | The 20 largest files largest first and the folders, each with its size; the old download with size, age and origin; the resized photo (not the one to keep) and the exact duplicate; both used installers; the proposal waits and no file moves |
| 4 | `list_installed_apps` (`unused_days` 180), `propose_uninstall` | Only the apps unused for 180 days or never used; each has last use with its source (or "no usage recorded" with no source and no date), a description ("unknown" when the knowledge base does not know it) and a verdict with its reason; the uninstall waits and no tray task exists before approval |
| 5 | `scan_suspicious`, `list_browser_extensions`, `list_network_processes` | Every persistence point has a status and a summary; only the Defender detection is `known_dangerous`, with its verdict source; every other item has no verdict and no "dangerous" word, suspicious ones their reasons; extensions with origin and the "probably not installed by you" flag; network talkers with trust, listeners and the 6 hours of background traffic; no startup change, Defender scan or VirusTotal lookup |
| 6 | `check_backups` | All seven sources checked; restore points listed but not counted; "no backup exists" with a recommendation naming the healthy drives L: and K:; the repositories without a remote or with unpushed or uncommitted work; once File History copies to K:, the last backup's source, location and age (9 days) |

### The audit-log check

Every prompt test ends with `expect_every_action_approved`: no `action_log` row may act without
the owner's approval of its plan in the tray. Bookkeeping rows are no action: a plan proposed or
put to the owner, a transition the policy engine refused, the owner's approval, rejection and
item decisions. Every other row (a plan approved, applying, applied, verified, rolled back or
failed; an item applied, restored or purged; a governor step, a pause, a manifest) needs an
earlier accepted transition of its plan to `approved` by `owner`, which only the tray's
`plans.approve` makes. A failure names the row: `plan 7: apply disabled by scheduler (row 12)`.
`audit_check_test.cpp` proves the check fails and names such a row (also an approval by an MCP
client and a governor step without a plan), and that no MCP tool can approve, execute or undo a
plan. The auto tier, which cleans on its own by design, would count as an action without a tray
approval; none of the prompts runs it, and the fixture has no auto-tier rule.

## The manual pass (before each release)

Run it on the installed release (`tools/deploy/deploy.ps1`, [deploy.md](deploy.md)) with the
service running. The repository's `.mcp.json` registers the server:

```json
"myclean": {"command": "C:\\Program Files\\MyClean\\current\\bin\\myclean.exe", "args": ["mcp"]}
```

For each prompt: start a new Claude Code session in the repository, check that `/mcp` lists
`myclean` as connected, paste the prompt exactly as written below, and tick the line when the
answer meets the pass condition. Then ask the same in the MyClean UI (tray icon, dashboard) and
tick that line. A prompt passes when the answer covers every data source listed for it, explains
each item, and nothing ran without your click in the tray.

| # | Prompt, as written | Pass when | Claude Code | MyClean UI |
|---|---|---|---|---|
| 1 | List every process above 5 % CPU or memory, explain each simply, flag what you don't recognize. Close nothing. | All qualifying trees listed with plain description, signer, trust flag; nothing closed | [ ] | [ ] Processes |
| 2 | Show everything that starts automatically, say which I need, disable only what I select. | All sources listed with impact; only approved items disabled; undo works | [ ] | [ ] none yet (see below) |
| 3 | Top 20 files and folders, old downloads, duplicate photos, installers already used. Sizes. Delete nothing without permission. | All four lists with sizes; nothing deleted without approval | [ ] | [ ] Space |
| 4 | Apps not opened in six months, what each does, which are safe to uninstall, which came with the PC. I decide. | Each app has last-used with source, description and verdict | [ ] | [ ] none yet (see below) |
| 5 | Unrecognized processes, browser extensions I didn't install, programs talking to the internet in the background. Known dangerous? Remove nothing. | Each item explained; "dangerous" only from a verdict; nothing removed | [ ] | [ ] none yet (see below) |
| 6 | Is there any backup, where, and when was the last one? If none, explain the simplest setup with what I have. | All sources checked; last-backup age; concrete recommendation | [ ] | [ ] none yet (see below) |
| 7 | On the 1st of each month: health check, storage analysis, intruder scan, then a short report of what changed. Make it a scheduled task. | Runs on day 1 or next boot; report diffs against previous month | [ ] | [ ] report toast, Open report |

For prompts 2 and 4, approve only some items in the tray (Cleanup section) and check that only
those changed; for prompt 2, undo the plan there and check the items are back. For prompt 7,
the "scheduled task" is MyClean's own monthly schedule, never a Windows scheduled task.

The UI shows no Startup, Apps, Security or Backups screen yet, and its Space section offers exact
duplicates only, not duplicate photos (prd.md UI-3). Until those screens exist, the UI lines of
prompts 2, 4, 5 and 6 cannot pass, and prompt 3's UI pass covers the exact duplicates only.

After the seven prompts, check that nothing ran without your approval: in Claude Code, ask for
`list_plans`; every plan past `awaiting_approval` must be one you approved in the tray. With a
`sqlite3` that opens the store read-only, this query must return no row (it is the suite's
check):

```sql
SELECT a.id, a.plan_id, a.event, a.to_state, a.outcome, a.actor FROM action_log a
WHERE NOT (a.event = 'transition' AND (a.outcome = 'rejected' OR
           a.to_state IN ('proposed', 'awaiting_approval') OR
           (a.to_state IN ('approved', 'rejected') AND a.actor = 'owner')))
  AND NOT (a.event = 'approval' AND a.actor = 'owner')
  AND NOT EXISTS (SELECT 1 FROM action_log b
                  WHERE b.plan_id = a.plan_id AND a.plan_id <> 0 AND b.id < a.id AND
                        b.event = 'transition' AND b.to_state = 'approved' AND
                        b.outcome = 'accepted' AND b.actor = 'owner');
```

```powershell
sqlite3 -readonly "$env:ProgramData\MyClean\myclean.db" "<the query above>"
```

On the live PC this also lists what the auto tier cleaned on its own and what the governor did,
which need no approval by design; only rows of plans the prompts proposed must be absent.
