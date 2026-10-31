## 1. Implementation
- [ ] 1.1 RP1: monthly snapshot of startup items, extensions, services, listening ports, unsigned binaries, top folders, disk health, plus 30-day aggregates; diff against the previous snapshot
- [ ] 1.2 RP2: monthly report on the 1st at 09:00 with catch-up at next boot: Markdown + HTML in %ProgramData%\MyClean\reports\YYYY-MM, toast with Open report, get_report and compare_snapshots MCP tools
- [ ] 1.3 Q1: seven-prompt acceptance suite: MCP tool calls against fixture machine state for prompts 1-7, plus a manual pass of each prompt in Claude Code
- [ ] 1.4 Audit every P0 requirement in docs/prd.md against its implementation and tests; fix gaps
- [ ] 1.5 CHANGELOG.md v1.0.0 entry

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
