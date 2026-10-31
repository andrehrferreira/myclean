# Proposal: phase1_see-everything

## Why
The owner cannot see what is slowing the machine, how healthy the 8 drives are, or why Ctrl+C/V freezes. Visibility must come first: it is read-only (no risk), answers prompt 1, and starts accumulating the history (process starts, freeze incidents, disk health) that later milestones depend on. Milestone M1 in docs/roadmap.md.

## What Changes
Windows service (SYSTEM) with named-pipe IPC; collectors for CPU, memory, every physical disk, volumes, GPU, processes (+ETW starts and network); telemetry pipeline with history; alert engine; NVMe/SATA disk-health reader with history and delta alerts; input/clipboard watchdog with incident classification; process trust signal + knowledge base v1; tray shell and dashboard; CLI with --json and `status`; MCP server with read-only tools. Work packages R1–R6, T1–T7, H1–H2, I1–I2, K1 and spikes X1–X2 in docs/dag.md.

## Impact
- Affected specs: runtime, telemetry, disk-health, alerts, input-watchdog, trust, cli-mcp, dashboard
- Affected code: src/svc, src/tray, src/cli, src/platform/{proc,etw,perf,disk,clipboard,input}, src/core/{analyze,rules}, data/kb
- Breaking change: NO
- User benefit: Live and historical view of CPU, memory, all 8 drives and processes; disk-failure warnings; data on Ctrl+C/V freezes; prompt 1 answerable
