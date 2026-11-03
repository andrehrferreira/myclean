# CLI and MCP Specification

## ADDED Requirements

### Requirement: CLI Read Commands
The CLI `myclean <cmd> [--json]` SHALL offer every M1 read feature (health, processes, CPU, memory, disks, volumes, disk health, alerts, freeze incidents) and, with `--json`, SHALL write a single machine-readable JSON document to stdout.

#### Scenario: Human output
Given the service is running
When the owner runs `myclean health`
Then a readable summary is printed and the exit code is 0

#### Scenario: JSON output
Given the service is running
When the owner runs `myclean health --json`
Then stdout holds one valid JSON document and nothing else

### Requirement: Status Command
`myclean status --json` MUST exit 0 when the service is reachable and healthy, MUST exit non-zero with a JSON reason otherwise, and MUST answer within 20 s (deploy contract C6).

#### Scenario: Healthy service
Given the service is running and answers on the pipe
When `myclean status --json` runs
Then it prints a JSON status and exits 0 within 20 s

#### Scenario: Service down
Given the service is stopped
When `myclean status --json` runs
Then it prints a JSON object whose reason says the service is unreachable and exits non-zero within 20 s

#### Scenario: Service hung
Given the service accepts the pipe connection but never answers
When `myclean status --json` runs
Then it gives up, prints a JSON reason stating a timeout and exits non-zero within 20 s

### Requirement: MCP Server
`myclean mcp` SHALL run an MCP server over stdio exposing read tools: `health_snapshot`, `explain_process(pid)`, and queries for volumes, physical disks and disk health.

#### Scenario: Tool discovery
Given Claude Code is configured to launch `myclean.exe mcp`
When it lists the server's tools
Then `health_snapshot`, `explain_process` and the volume, disk and disk-health query tools are listed, each with an input schema

### Requirement: Read Tools Change Nothing
The MCP read tools and CLI read commands MUST NOT change any system or MyClean state: they MUST NOT close, suspend or reprioritize a process, create a plan, or write to the action log; in this phase the MCP server SHALL expose no proposal or action tools.

#### Scenario: Prompt 1 closes nothing
Given the audit log has N entries
When an MCP client calls every read tool, including `health_snapshot` and `explain_process`
Then no process was closed or changed and the audit log still has N entries

#### Scenario: Asked to close a process
Given an MCP client is connected
When it asks the server to terminate a process
Then no tool exists for it and the request fails without effect

### Requirement: Health Snapshot
`health_snapshot` and `myclean health` SHALL list every process tree whose CPU exceeds 5 % (normalized to all logical processors) or whose private working set exceeds 5 % of physical memory, each with name, plain description or "unknown", signer, path and trust flag, plus the top consumers of the last 24 h and system signals (commit charge, C: free %, disk queue length), within 3 s.

#### Scenario: Tree above threshold
Given a browser tree uses 9 % CPU across all 32 logical processors
When `health_snapshot` is called
Then the tree is listed once, expandable to its members, with description, signer, path and trust flag

#### Scenario: Below both thresholds
Given a process tree uses 2 % CPU and 1 % of memory
When `health_snapshot` is called
Then it is not in the list of current qualifying trees

#### Scenario: Unknown process flagged
Given a qualifying process tree has no knowledge-base match and is unsigned
When `health_snapshot` is called
Then it is listed with description "unknown" and flagged unrecognized with its reasons

#### Scenario: Answer time
Given the service is running on the reference machine
When `health_snapshot` is called
Then the result arrives in less than 3 s

### Requirement: Explain Process
`explain_process(pid)` SHALL return, for a running process, its tree, current resource use, knowledge-base description, category, signer, path and trust signal with reasons.

#### Scenario: Known process
Given PID 1234 belongs to a process in the knowledge base
When `explain_process(1234)` is called
Then the answer contains the plain description, category, signer, path, trust signal and current CPU and memory

#### Scenario: PID not running
Given no process has PID 99999
When `explain_process(99999)` is called
Then the tool returns an error stating the process was not found

### Requirement: Degraded Mode
When the service is unreachable, the CLI and the MCP server SHALL still answer requests that need only per-user data and SHALL state, for every other request, that it needs the service.

#### Scenario: MCP while the service is stopped
Given the service is stopped
When an MCP client calls `health_snapshot`
Then the result states that live telemetry and disk health are unavailable because the service is not running, and the server keeps running

### Requirement: Prompt 1 Acceptance
Pasting owner prompt 1 into Claude Code with `myclean mcp` registered SHALL produce an answer that lists every qualifying process tree with a plain explanation, signer and trust flag, flags unrecognized processes, and closes nothing.

#### Scenario: Prompt 1 in Claude Code
Given `myclean mcp` is registered in `.mcp.json` and the service is running
When the owner pastes prompt 1 as written
Then the answer covers every tree returned by `health_snapshot` with description, signer and trust flag, and the audit log shows no new action
