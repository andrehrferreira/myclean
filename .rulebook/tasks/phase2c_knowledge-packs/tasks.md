## 1. Implementation
- [x] 1.1 Design docs/packs.md: layout, manifest schema, case kinds, applies_when, id namespacing, conflict and precedence rules, owner overrides, versioning, how to add a case + test it
- [x] 1.2 core/packs: discovery, manifest + content validation (reject whole pack with file/case/field errors), applies_when evaluation behind interfaces, namespaced registry feeding RuleCatalog, attribution, KB, classes and new relocation/compression recipe kinds
- [x] 1.3 Generic relocation recipe kind (env var, tool config file key, registry value, CLI move command like `wsl --manage --move`) and compression target kind, consumed by placement and compress modules
- [x] 1.4 Per-pack fixture tests: each pack may ship tests/*.toml (fake file trees + expected matches/non-matches); one ctest per pack discovered automatically
- [x] 1.5 New packs from the real C: map: zig, playwright, vcpkg, wsl-docker (VHDX relocate/compact), ai-tools (codex/claude sessions, ollama/lmstudio/local models), chrome on-device model + profile caches, vscode web storage + VSIX cache, android sdk/avd, python store, rust toolchains, windows setup logs (Panther)
- [ ] 1.6 Migrate existing data (rules, attribution, kb, classes, drive-specs) into packs without behaviour change (all existing tests green)
- [ ] 1.7 `myclean packs list|validate [path]` and pack info in status/MCP
- [ ] 1.8 Owner override packs in %ProgramData%\MyClean\packs (admin-writable only), enabled/disabled per pack

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
