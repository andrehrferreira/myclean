# Proposal: phase0_foundation

## Why
MyClean deletes files, changes system settings and throttles processes as SYSTEM. Before any feature exists, the project needs a build and quality gate, a core/platform split that makes decision logic testable without a live machine, and a single safety layer (SafeRemove, action framework, audit log) that every later destructive feature must go through. Milestone M0 in docs/roadmap.md.

## What Changes
Repository skeleton (CMake + Ninja + vcpkg, MSVC /W4 /WX, clang-tidy, clang-format, GoogleTest, GitHub Actions gate, presets and install layout per deploy contracts C1–C2); core model and platform interfaces with fakes; SQLite store with migrations and time-series downsampling; schema-validated loaders for rules, process classes and the knowledge base; SafeRemove with deny-list and reparse/cloud guards; action framework with policy state machine and append-only audit log; in-app scheduler with catch-up. Work packages F1–F7 in docs/dag.md.

## Impact
- Affected specs: build, safety, store, actions, scheduler
- Affected code: CMakeLists.txt, CMakePresets.json, vcpkg.json, src/core/*, src/platform/ (interfaces), tests/*, .github/workflows/*, .clang-format, .clang-tidy
- Breaking change: NO
- User benefit: Every later milestone builds on a tested safety core and a green CI gate
