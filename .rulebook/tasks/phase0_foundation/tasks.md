## 1. Implementation
- [x] 1.1 F1: git init; CMakeLists + CMakePresets (configure/build/test presets `release` and `debug`, binaryDir ${sourceDir}/build/${presetName}) + vcpkg manifest; MSVC /W4 /WX /permissive-; install() places executables/DLLs/PDBs in bin/ and data files in data/ (deploy contracts C1, C2)
- [x] 1.2 F1: .clang-format, .clang-tidy and a `lint` build target; GoogleTest wired into ctest
- [x] 1.3 F1: GitHub Actions windows-latest workflow running configure -> build -> lint -> ctest; pre-commit hook running type-check + lint + affected tests
- [x] 1.4 F2: core model types (ids, units, Sample, Series, Finding, Plan, Action, Undo) in src/core with no <windows.h>; platform interfaces (IFileSystem, IProcessTable, IJobController, IRegistry, IClipboardProbe, ...) with fakes in tests
- [ ] 1.5 F3: SQLite store in WAL mode with versioned migrations, integrity check at open, repositories, batched writes (10 s) and synchronous audit writes
- [ ] 1.6 F3: time-series pipeline storage: in-memory 1 s ring (1 h), minute aggregates (30 d), hour aggregates (2 y), retention jobs
- [x] 1.7 F4: TOML/JSON loaders for data/rules/*.toml, data/classes.toml, data/kb/*.json with schema validation; an invalid file is rejected whole, never half-applied
- [ ] 1.8 F5: SafeRemove: hard deny-list, never follows reparse points, skips cloud placeholders (RECALL_ON_DATA_ACCESS / RECALL_ON_OPEN / OFFLINE), accepts only items of a stored plan, re-validates each item immediately before acting
- [ ] 1.9 F5: SafeRemove tests: one per deny-list entry, junction/symlink, cloud placeholder, TOCTOU swap, item not in plan
- [ ] 1.10 F6: Action contract (describe/preview/apply/verify/undo), plan state machine (proposed -> auto|awaiting_approval -> approved -> applying -> applied -> verified|rolled_back|failed), actors (scheduler, governor, owner, mcp:*) with mcp:* unable to approve
- [ ] 1.11 F6: append-only action_log (DB trigger rejects UPDATE/DELETE) with undo blobs; quarantine table
- [x] 1.12 F7: in-app scheduler: cron-expression parser, persisted last/next run, catch-up of missed runs after downtime, no OS scheduler
- [ ] 1.13 Run tools/deploy/deploy.ps1 end to end: gate passes, and with no executable yet it stops with a clear message

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
