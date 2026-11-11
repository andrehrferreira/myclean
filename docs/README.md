# MyClean documentation

| Document | Purpose |
|---|---|
| [prd.md](prd.md) | What MyClean must do: problem, goals, requirements (MON, HLT, SPC, CLN, PLC, GOV, OPT, INP, SEC, APP, BAK, RPT, UI), NFRs, prompt acceptance |
| [architecture.md](architecture.md) | How it is built: processes, layers, collectors, storage, action framework, governor, IPC/MCP, security, testing |
| [roadmap.md](roadmap.md) | Milestones M0–M6 to v1.0, spikes, exit criteria |
| [dag.md](dag.md) | Work-package dependency graph, critical path, parallel lanes |
| [acceptance.md](acceptance.md) | The seven-prompt acceptance suite: what each prompt test checks, how to run it, and the manual pass before a release |
| [deploy.md](deploy.md) | Installing and hot-updating MyClean on this PC with `tools/deploy/deploy.ps1`, and the contracts the binaries must honor |
| [analysis/myclean-planning/](analysis/myclean-planning/README.md) | The initial analysis with measurements and findings F-001 … F-043 (evidence behind the PRD) |

Order of authority: PRD → architecture → roadmap/DAG. The analysis is
supporting evidence. Where its execution plan (file 08) differs, the roadmap
and the DAG take precedence.
