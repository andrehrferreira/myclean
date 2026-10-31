# Build Specification

## ADDED Requirements

### Requirement: CMake Presets Contract
The repository MUST provide a `CMakePresets.json` with configure, build and test presets named `release` and `debug`, each with `binaryDir` set to `${sourceDir}/build/${presetName}` (deploy contract C1).

#### Scenario: Release preset builds and tests from a clean checkout
Given a clean checkout on Windows 11 with MSVC, Ninja and vcpkg available
When `cmake --preset release`, `cmake --build --preset release` and `ctest --preset release` are run in order
Then all three commands exit 0
And the build tree is `build/release` under the source directory

#### Scenario: Debug preset is available
Given the same checkout
When `cmake --list-presets=all` is run
Then configure, build and test presets named `debug` are listed alongside `release`

### Requirement: Dependency Manifest
The build MUST resolve every third-party library through a vcpkg manifest (`vcpkg.json`) and MUST NOT depend on libraries installed globally on the build machine.

#### Scenario: Dependencies come from the manifest
Given a machine with no SQLite, toml++, nlohmann/json, spdlog or GoogleTest installed outside vcpkg
When the `release` preset is configured
Then vcpkg installs the manifest dependencies and configuration succeeds

### Requirement: Warnings Are Errors
Every first-party translation unit MUST compile with MSVC `/W4 /WX /permissive-`, so that any compiler warning fails the build.

#### Scenario: A new warning breaks the build
Given a first-party source file that introduces an unreferenced local variable
When the project is built with either preset
Then the build fails with that warning reported as an error

### Requirement: Relocatable Install Layout
`cmake --install` MUST produce a complete, relocatable tree with executables, DLLs and PDBs in `bin/` and data files (`rules/`, `classes.toml`, `kb/`) in `data/` (deploy contract C2).

#### Scenario: Install tree layout
Given a successful `release` build
When `cmake --install build/release --prefix out/stage` is run
Then `out/stage/bin` contains every built executable with its DLLs and PDBs
And `out/stage/data` contains `rules/`, `classes.toml` and `kb/`
And no installed file depends on the build directory's absolute path

#### Scenario: Installed tree runs from another location
Given an install tree copied to a different directory
When an installed executable resolves its data directory
Then it finds the data at `..\data` relative to its own location

### Requirement: Lint and Format Targets
The build MUST provide a `lint` target that runs clang-tidy with the repository `.clang-tidy` over first-party sources and fails on any finding, and the repository MUST carry a `.clang-format` that defines the code style.

#### Scenario: Lint finding fails the target
Given a first-party source file that violates an enabled clang-tidy check
When the `lint` target is built
Then the target fails and names the file, line and check

#### Scenario: Clean code passes lint
Given first-party sources with no clang-tidy findings
When the `lint` target is built
Then it exits 0

### Requirement: Unit Tests Registered with CTest
All unit tests MUST be written with GoogleTest and registered with CTest so that `ctest --preset <name>` runs the full suite, and the `debug` test run MUST be built with AddressSanitizer.

#### Scenario: Failing test fails ctest
Given a GoogleTest case that fails
When `ctest --preset release` is run
Then ctest exits non-zero and reports the failing test by name

#### Scenario: Memory error caught in debug
Given a unit test that reads past the end of a heap buffer
When `ctest --preset debug` is run
Then AddressSanitizer reports the error and the test fails

### Requirement: Core Has No Platform Dependency
Code under `src/core` MUST NOT include `<windows.h>` or any Win32 header, and MUST reach the operating system only through platform interfaces (`IFileSystem`, `IProcessTable`, `IJobController`, `IRegistry`, `IClipboardProbe` and others) for which test fakes exist under `tests/`.

#### Scenario: Win32 include in core breaks the gate
Given a file under `src/core` that includes `<windows.h>`
When the build or the `lint` target runs
Then it fails and names the offending include

#### Scenario: Core logic tested with fakes
Given a core component that lists files through `IFileSystem`
When its unit test runs with the fake file system
Then the test passes without touching the real disk

### Requirement: CI Quality Gate
A GitHub Actions workflow on `windows-latest` MUST run configure, build, lint and ctest in that order on every push and pull request, and MUST fail if any step fails.

#### Scenario: Red step fails the workflow
Given a pull request whose code fails the `lint` target
When the workflow runs
Then the lint step fails, the ctest step does not run, and the workflow status is failed

#### Scenario: Green change passes
Given a change that builds without warnings, has no lint findings and passes every test
When the workflow runs
Then all four steps succeed and the workflow status is passed

### Requirement: Pre-Commit Hook
The repository MUST provide a pre-commit hook that runs the type-check (compile), lint and the tests affected by the change, and blocks the commit when any of them fails.

#### Scenario: Commit blocked on failing test
Given a staged change that breaks a unit test it affects
When the developer runs `git commit`
Then the hook fails and no commit is created

### Requirement: Deploy Script Compatibility in M0
Running `tools/deploy/deploy.ps1` against the M0 tree MUST pass the build and test gate and then stop with a clear message stating that no executable exists yet, without changing the installed system.

#### Scenario: Deploy with no executable
Given the M0 tree with a green build and test suite and no service, tray or CLI executable
When `pwsh -File tools\deploy\deploy.ps1` is run
Then configure, build and ctest succeed
And the script stops with a message that there is no executable to deploy
And no service, `current` junction or HKCU Run entry is created or changed
