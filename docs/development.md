# Development

How to build, test and extend MyClean. Design: [architecture.md](architecture.md).
Deployment on this PC: [deploy.md](deploy.md).

## Prerequisites

- Windows 11, Visual Studio 2022 (or Build Tools) with **Desktop development with C++** and
  **C++ Clang tools for Windows** (clang-tidy and clang-format; the build uses the copies that
  ship with the Visual Studio that provides the compiler).
- CMake 3.25 or newer and Ninja, on `PATH`.
- vcpkg. The presets use `$env:VCPKG_ROOT`; when it is unset they fall back to `C:\vcpkg`
  (`cmake/vcpkg-toolchain.cmake`). Dependency versions are pinned by `builtin-baseline` in
  `vcpkg.json`, so any vcpkg clone that contains that commit works.
- Run everything from a **developer shell** (x64), so `cl.exe` is on `PATH`:
  `Developer PowerShell for VS 2022`, or in any pwsh:

  ```powershell
  $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
  Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
  ```

## Build and test

| Preset | What it is |
|---|---|
| `debug` | Debug build with AddressSanitizer; the run that catches memory errors |
| `release` | Optimized build with PDBs; what `tools/deploy/deploy.ps1` builds and installs |

```powershell
cmake --preset debug                    # configure; the first run builds the vcpkg dependencies
cmake --build --preset debug            # build (warnings are errors)
ctest --preset debug                    # all unit tests
ctest --preset debug -L model           # one module's tests (label = module name)
cmake --build --preset debug --target format-check   # clang-format, fails on any difference
cmake --build --preset debug --target format         # rewrite files in the repository style
cmake --build --preset debug --target lint           # clang-tidy, fails on any finding
pwsh -File tools/dev/check.ps1          # all of the above, as the pre-commit hook runs it
```

Build trees live in `build/<preset>` (deploy contract C1). `cmake --install build/release
--prefix <dir>` produces the relocatable tree: executables and PDBs in `bin/`, the `data/`
folder of the repository in `data/` (contract C2).

## Layout

```
src/core/<module>/      pure C++20 libraries; no Win32 header (checked on every build)
src/core/model/         shared vocabulary: ids, units, Result, Finding, Plan, Action, Actor
src/core/os/            interfaces to the OS (IFileSystem, IClock) + the portable SystemClock
src/platform/<module>/  Win32 implementations of src/core/os interfaces
tests/fakes/            in-memory fakes of the src/core/os interfaces (myclean::fakes)
tests/unit/<module>/    GoogleTest suite of one module
cmake/                  helper functions, lint/format targets, vcpkg toolchain
```

`src/`, `src/core/`, `src/platform/`, `tests/` and `tests/unit/` add every subfolder that has
a `CMakeLists.txt` automatically. **Adding a module never edits a shared file.**

## Adding a module and its tests

1. Create `src/core/<module>/CMakeLists.txt`:

   ```cmake
   myclean_add_library(<module>
     SOURCES
       thing.cpp
       thing.hpp
     DEPENDS
       myclean::model             # other modules: myclean::os, myclean::store, ...
       unofficial::sqlite3::sqlite3)
   ```

   This defines `myclean_<module>` with the alias `myclean::<module>`: a static library when
   SOURCES has a `.cpp`, header-only otherwise. Headers are included from the `src/` root:
   `#include "core/<module>/thing.hpp"`.

2. Create `tests/unit/<module>/CMakeLists.txt`:

   ```cmake
   myclean_add_test(<module>
     SOURCES
       thing_test.cpp
     DEPENDS
       myclean::<module>
       myclean::fakes)            # FakeFileSystem, FakeClock
   ```

   This builds `myclean_<module>_tests`, links GoogleTest's `main`, and registers every test
   with CTest under the label `<module>`.

3. Reconfigure is automatic: the next `cmake --build` sees the new folders.

A platform module is the same recipe under `src/platform/<module>/`, named
`platform_<module>` (target `myclean::platform_<module>`), with Win32 headers allowed.
An executable uses `add_executable` plus `myclean_install_executable(<target>)`, which installs
the binary and its PDB into `bin/`. In the `debug` preset (`MYCLEAN_ASAN=ON`) every executable
must also get the POST_BUILD copy of `${MYCLEAN_ASAN_RUNTIME}` next to it, as
`myclean_add_test` does. Otherwise starting it outside a developer shell raises a
"clang_rt.asan_dynamic-x86_64.dll not found" system dialog.

### Third-party libraries

Every dependency is declared in `vcpkg.json` and found once in the root `CMakeLists.txt`, so
modules only link the targets:

| Library | Target |
|---|---|
| SQLite | `unofficial::sqlite3::sqlite3` |
| toml++ | `tomlplusplus::tomlplusplus` |
| nlohmann/json | `nlohmann_json::nlohmann_json` |
| spdlog | `spdlog::spdlog` |
| tl::expected | `tl::expected` (already public through `myclean::model`) |
| WIL (platform only) | `WIL::WIL` |
| GoogleTest | linked by `myclean_add_test` |

Third-party code is linked statically (triplet `x64-windows-static-md`), so no third-party DLL
has to be deployed. `src/core` may include only C++ standard headers, `"core/..."` headers and
the portable libraries listed in `cmake/scripts/check-core-includes.cmake`; anything else (any
Win32 header) fails the build with the file and line.

## Code conventions

- **Namespaces:** model types live directly in `myclean` (`myclean::Plan`,
  `myclean::Result`). Every other module uses `myclean::<module>` (`myclean::os`,
  `myclean::store`); fakes use `myclean::fakes`.
- **Naming** (enforced by clang-tidy): types `CamelCase`, interfaces `IName`; functions,
  variables and enumerators `snake_case`; private members end in `_`.
- **Errors:** every fallible operation returns `Result<T>` (`Result<void>` for none), built
  with `fail(Errc::..., "message")`. `Errc` is a closed list of categories in
  `core/model/result.hpp`; put specifics in the message. Exceptions do not cross module
  boundaries; fakes may throw `std::logic_error` on misuse in test setup only.
- **Enums that are stored** have `to_string()` and `parse_<enum>()` with fixed text forms
  (`PlanState::automatic` is `"auto"`).
- **Paths** are `std::filesystem::path`. Convert to UTF-8 with `path.u8string()`, never with
  `path.string()` (that uses the ANSI code page).
- **Ids** are `Id<Tag>`; a module defines its own (`using RunId = Id<struct RunTag>;`).
- **Time:** `WallTime` for stored timestamps, `MonoTime` for intervals, both from an injected
  `os::IClock`; tests use `fakes::FakeClock` and never sleep.
- **OS access** from core only through `src/core/os` interfaces; tests use the fakes. Platform
  interfaces for later phases (process table, jobs, registry, clipboard) are added with the
  phase that needs them.

### Result type: tl::expected on C++20

`Result<T>` is `tl::expected<T, Error>` (vcpkg `tl-expected`), not C++23 `std::expected`.
The architecture fixes C++20 (section 10), and MSVC offers `std::expected` only in its
`/std:c++latest` preview mode. `tl::expected` has the same interface (`has_value`, `value`,
`error`, `and_then`, `transform`), so moving to `std::expected` later is a rename in
`result.hpp`.

## Quality gate

| Check | Where | Fails on |
|---|---|---|
| Build | every build | any MSVC warning (`/W4 /WX /permissive-`) |
| Core isolation | every build and `lint` | a non-portable include under `src/core` |
| `format-check` | hook, CI | any clang-format difference (`.clang-format`) |
| `lint` | hook, CI | any clang-tidy finding (`.clang-tidy`, warnings are errors) |
| Tests | hook, CI, deploy | any failing test; `debug` runs under AddressSanitizer |

Silence a clang-tidy finding only with `// NOLINT(<check>): <reason>` on the line.
Under AddressSanitizer the STL container annotations are disabled
(`_DISABLE_STRING_ANNOTATION`, `_DISABLE_VECTOR_ANNOTATION`) because the vcpkg libraries are
built without ASan and MSVC refuses to link mixed annotations; every other ASan check is on.

### Pre-commit hook

Enable it once per clone:

```powershell
git config core.hooksPath .githooks
```

`.githooks/pre-commit` runs `tools/dev/check.ps1`: build, `format-check`, `lint` and the full
test suite of the `debug` preset (set `MYCLEAN_HOOK_PRESET=release` to use release). The hook
checks the working tree, so stage everything you intend to commit. Never bypass it with
`--no-verify`.

### CI

`.github/workflows/ci.yml` runs on `windows-latest` for `debug` and `release`: configure,
build, format-check, lint, ctest. vcpkg is cloned at the `builtin-baseline` commit and its
binary archives are cached.
