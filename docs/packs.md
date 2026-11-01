# Knowledge packs

What MyClean knows about where space goes and how to get it back lives in data, grouped in
**packs**: one pack per tool or ecosystem (`zig`, `vcpkg`, `chrome`, ...). Adding a case, or a
whole new ecosystem, is a data change with its own tests; no code changes.

Code: `src/core/packs` (loader, validation, applicability, registry, fixture runner),
`src/platform/packs` (the machine `applies_when` asks about, the owner folder's ACL check),
`src/cli/packs_command.cpp` (`myclean packs`). Tests: `tests/unit/packs`, `tests/unit/platform_packs`. Task: `.rulebook/tasks/phase2c_knowledge-packs`.

## Layout

```
data/packs/<pack-id>/
  pack.toml          manifest (required)
  clean.toml         cleaning rules          ┐
  attribution.toml   disk map areas          │
  relocate.toml      relocation recipes      │ at least one
  compress.toml      compression targets     │
  kb.json            process knowledge base  │
  classes.toml       governor classes        ┘
  tests/*.toml       fixtures (every shipped pack has at least one)
```

No other file may be in the pack folder: a typo such as `clean.tml` rejects the pack instead of
being ignored.

## The manifest: pack.toml

```toml
id          = "zig"                    # required: a-z, 0-9 and inner '-'; the folder's name
version     = "1.0.0"                  # required: MAJOR.MINOR.PATCH, the pack's own version
title       = "Zig"                    # required
description = "Zig's global build cache ..."               # required, plain English
sources     = ["https://...", "observed on reference machine: ..."]   # required
min_myclean = "0.1.0"                  # required: the oldest MyClean that can load the pack

[applies_when]                         # optional: absent = applies on every machine
all = [{ os_build_min = 22621 }]       # every condition must hold, and
any = [{ path_exists = '%LOCALAPPDATA%\zig' }, { app_installed = '^Zig ' }]
                                       # at least one must (when `any` is given)
```

A condition sets exactly one of:

| Condition | Holds when |
|---|---|
| `app_installed` | the regular expression (ECMAScript, any case) matches the DisplayName of an installed program (Uninstall keys of HKLM, its 32-bit view and HKCU) or an installed Appx package name |
| `path_exists` | the path exists after `%VARIABLE%` expansion; `*` and `?` match within one component; false when a variable is not set. Needs a real drive or variable (no `?:\`) |
| `process_image` | a running process has this image name (`ollama.exe`, any case) |
| `os_build_min` | the Windows build number is at least this |

A pack that does not apply is loaded and validated but contributes nothing to the engines on
that machine (`packs::make_registry(packs, probes)`).

**Sources.** Every pack lists where its knowledge comes from: official documentation, or
`observed on reference machine: <what>` for facts read on the owner's PC (folder layouts,
registry values). Each content file repeats, in a comment above each case, which source backs
it. Do not add a case nobody can back.

## Case kinds

Every content file is validated whole by its kind's parser; the existing kinds keep their
existing schema, documented next to their types.

| File | Entries | Schema | Consumed by |
|---|---|---|---|
| `clean.toml` | `[[rule]]` | `rules::CleaningRule` (`src/core/rules/schema.hpp`) | `clean::RuleEngine` |
| `attribution.toml` | `[[area]]` | `map::Area` (`src/core/map/attribution.hpp`) | `myclean map` |
| `kb.json` | `{"entries": [...]}` | `rules::KbEntry` (`src/core/rules/schema.hpp`) | trust |
| `classes.toml` | `[[match]]` | `rules::ClassMatch`, without `[caps]` (job caps are global) | governor |
| `relocate.toml` | `[[relocate]]` | `packs::RelocationRecipe` (below) | placement |
| `compress.toml` | `[[compress]]` | `packs::CompressTarget` (below) | compress |

A clean target is an absolute path pattern (starting with a `%VARIABLE%`, a drive such as
`C:\`, or `?:\`, which stands for the same path on every local fixed volume) or, in a rule
with `project`, a pattern relative to the project root. A target that could never match
(relative without `project`, absolute with it, or with a `..` component) rejects the pack.
A project rule with `require.git_ignored` may add `require.outside_git = true` when its targets
are written only by a tool: a project outside any git work tree is then offered on the other
conditions, while inside one git must still ignore the folder and track nothing in it.

### relocate.toml: relocation recipes

Moving something to another drive through the tool's own supported setting or command, never a
junction.

```toml
[[relocate]]
id          = "rustup-home"
label       = "Rust toolchains (RUSTUP_HOME)"
locations   = ['%USERPROFILE%\.rustup']        # where the content usually is; '?:\' allowed
current     = { env = "RUSTUP_HOME" }           # where the current location is read
target_kind = "env"                             # how the new location is set
content     = "move"                            # move | regenerate
needs       = ["rustup.exe", "cargo.exe"]       # images that must not run meanwhile
undo        = "restore"
note        = "..."                             # optional; required with undo = "none"
```

`current` is exactly one of:

| `current` | The location is |
|---|---|
| `{ env = "NAME" }` | the owner's environment variable |
| `{ config = 'path', key = "k", format = "ini" \| "json" }` | a key of a tool's configuration file (`ini`: `key = value` lines such as `.npmrc`; `json`: a top-level key) |
| `{ registry = 'HKCU\...\Value', name = "Value" }` | a registry value (HKCU or HKLM). A `*` in the key matches every subkey: each is one instance, named by its value `name` (in the same key). `where = { value = "Flags", mask = 8, equals = 8 }` keeps only the instances whose DWORD value `value`, masked, equals `equals`; an instance without it is not offered |
| `{ command = "..." }` | what a command prints |

`target_kind` is `env`, `config` or `registry` (then it must equal the kind of `current`: the
same setting is written), `command`, which runs `command` with `{target}` (the new folder,
required) and `{name}` (the instance name, only with `current.name`; each placeholder stays
inside one argument), or `wsl-import` (instances required), which moves a WSL 1 distribution:
`wsl --export <name> <target>\<name>.tar`, `wsl --import <name>-myclean-move <target> <archive>
--version 1`, a check (its Lxss key has BasePath = the target and `wsl -d <temporary> -e true`
succeeds), only then `wsl --unregister <name>`, after which the copy gets the original
DistributionName and DefaultUid. A failed import or check removes the copy and leaves the original
registered; the archive stays as a backup. `skip_names` lists instances that are never offered.
The instance name given to a command is the one read from the registry, never the approved
text.

`undo` is `restore` (the previous value is written back exactly, deleting a setting that was
absent; env, config and registry only), `command` (the move runs again with `{target}` = the
previous folder; `command` and `wsl-import`) or `none` (the `note` says how to go back).

`content`: `move` copies and verifies before the setting changes; `regenerate` leaves the old
content behind for a cleaning plan, the tool rebuilds what it needs.

**Approval.** A recipe never runs by itself: `plans.propose_optimization` with `kind = "relocate"`,
`pack` and `recipe` (the case id) and, for instances, `instance` creates an approval-only
user-scope plan `RelocateRecipe` (target `<drive>\Relocated\<pack>\<case>[\<instance>]` on the
best healthy volume, or the drive given in `target`); the tray runs it with the owner's token,
with exact undo where the recipe allows it (`placement::RecipeAction`). In `wsl-docker`, `distro`
(Lxss `Flags` bit 0x8 set: WSL 2) uses `wsl --manage <name> --move`, `distro-files` (bit clear:
WSL 1) uses `wsl-import`. Lxss `Version` is the registration format, not the WSL version.

### compress.toml: compression targets

```toml
[[compress]]
id        = "toolchains"
label     = "Rust toolchains"
paths     = ['%USERPROFILE%\.rustup\toolchains']   # folders; a real drive or variable
algorithm = "lzx"                                  # xpress4k | xpress8k | xpress16k | lzx
exclude   = ["*.pdb"]                              # optional: names left alone inside
note      = "..."
```

The algorithm is a hint for the folder's content; the compression analyzer still estimates it,
and its own exclusions (media, disk images, databases, recent files, %WINDIR%) always apply.
`compress::AnalyzerOptions::targets` takes the registry's targets: a folder inside one gets its
algorithm as the recommendation (`FolderEstimate::target` names the case), and the names in
`exclude` are left alone (`Exclusion::pack_excluded`, which `CompressFolder` honours too).

## Ids and references

Every case id is local to its file (`[a-z0-9._-]`, unique per file) and is exposed as
`<pack>/<case>`: `zig/global-cache`. Two kinds may use the same local id (a clean case and the
area that points at it usually do).

An area's `reclaim.ref` names, for `kind = "clean-rule"`, a clean case **of the same pack** by
its local id (`ref = "global-cache"` becomes `zig/global-cache`). For `kind = "relocate"` it
names a relocation recipe of the same pack, or else a built-in relocation target
(`placement::CacheKind` text such as `cargo`, or `wsl`, `docker`), which is kept as written.
References into another pack are rejected. `handler` and `setting` refs are unchanged.

## Loading, precedence and conflicts

`packs::load_packs(files, builtin_dir, owner_dir, running)`:

1. **Owner packs first**: `%ProgramData%\MyClean\packs\<pack-id>\`, the owner's overrides. The
   folder must be writable by administrators only: `platform::owner_packs_folder` checks it and
   everything in it (owner SYSTEM, Administrators or TrustedInstaller; no allow entry giving
   anyone else write, delete or permission rights; no link) and otherwise refuses it, so the
   owner's packs are ignored and the refusal is reported. A missing folder is no error.
   (`%ProgramData%\MyClean` itself gets that ACL when the service starts.)
2. **Built-in packs**: `<install>\data\packs\<pack-id>\`, each origin in id order.

Rules:

- **Disable**: `%ProgramData%\MyClean\packs\packs.toml` holds `disabled = ["chrome", ...]`; a
  disabled id is skipped in both places.
- **Override**: an owner pack replaces the built-in pack with the same id (the built-in one is
  `shadowed`). An owner pack that is rejected does not: the built-in one stays in effect.
- **Whole-pack rejection**: any schema error, unknown file, id mismatch, bad case id, dangling
  ref or `[caps]` rejects the pack with one error naming the pack, the file, the case and the
  field, e.g.
  `pack 'zig': C:\...\packs\zig\clean.toml: rule #1 'global-cache': field 'tier' is missing`.
  Every other pack still loads. A pack needing a newer MyClean (`min_myclean`) is refused the
  same way.
- **Conflicts**: every case claims what it acts on, compared case-insensitively: each clean
  target (with its project marker), native command and cleanup handler; each attribution path;
  a relocation recipe's setting; each compression path; a knowledge-base image with its path
  and signer; a class's criteria. Two cases of one pack claiming the same thing reject the pack.
  A pack claiming what an already loaded pack claims is rejected whole, naming both cases; so
  owner packs win over built-in ones, and within one origin the first id wins. Overlap that is
  not identical is not a conflict: between attribution areas the most specific location wins
  (the disk map's rule), and the cleaning engine proposes an entry selected twice once.

The report lists every pack folder with its status (`loaded`, `disabled`, `shadowed`,
`rejected` and why) for `myclean packs list` and the service status.

`packs::make_registry` then gives the content of the loaded packs, per kind and in load order,
in the types the engines take: `clean` (a `RuleCatalog`-style list for `RuleEngine`),
`attribution`, `relocate`, `compress`, `kb` and `classes` (a `ClassSet`, first match wins,
default caps). `packs::load_installed(files, data_dir, owner_dir)` is `load_packs` of an
installed MyClean (`<data_dir>\packs`, the owner folder as `platform/packs` found it, this
MyClean's version).

## Where packs are used

| Consumer | Loads | Applicability |
|---|---|---|
| Service, `cleaning` | clean cases for scans, relocation recipes for `plans.propose_optimization`; again at every scan | for the owner: `platform::Win32Machine` (Uninstall keys of HKLM, its 32-bit view and the owner's hive, the owner's Appx packages, running images, the Windows build) and the owner's environment |
| Service, `trust` | knowledge base, once at start | none: every loaded pack (knowing a program that is not installed costs nothing) |
| `myclean map` | attribution areas | for the user running it |
| `myclean packs list` / `validate` | everything | `list`: for the user running it |
| Compression analyzer | `compress::AnalyzerOptions::targets` | the caller's registry |
| Governor | `Registry::classes` | `core/governor` classifier (pack `process-classes`); not wired into the service yet |

`myclean packs list [--json] [--data <dir>]` prints every pack folder with its version, origin,
status, whether it applies here and its case counts, and why a pack was rejected.
`myclean packs validate [<path>] [--json]` validates one pack folder, or every pack of a folder
of packs together (default: the installed `data\packs`, so conflicts show), and runs each pack's
fixtures on an in-memory machine; it exits 1 when a pack is rejected, has no fixture or a fixture
fails. The service's `status` lists every pack folder (`packs`) as the cleaning component last
loaded it; a rejected pack also makes the component degraded.

## Versioning

- `version` is the pack's own: bump PATCH for a fix that changes no selection (a label, a
  note, a source), MINOR for a new case or a wider selection, MAJOR for a removed or renamed
  case id (ids are stored in plans and logs).
- `min_myclean` is the oldest MyClean whose loader understands every field the pack uses: raise
  it when a pack starts using a field or kind added in a later MyClean.

## Testing a case

Each pack ships fixtures in `tests\*.toml`: a fake machine and what each case must and must not
select on it. ctest runs one test per pack folder, found when ctest starts
(`Packs/ShippedPack.IsValidAndPassesItsFixtures/<pack>`, label `packs`), so a new pack needs no
build change; `ShippedPacks.LoadTogetherWithoutConflicts` loads all packs together. The runner
is `packs::run_fixtures` (`src/core/packs/fixtures.hpp`), the same one `myclean packs validate`
uses, so a pack can be checked without a build.

```toml
volumes = ['C:\', 'F:\']                # optional, before any table: the fixed volumes a
                                        # '?:\' clean target stands for (default: every drive
                                        # the entries are on)

[env]                                   # optional, on top of the reference owner's environment:
VCPKG_ROOT = 'F:\tools\vcpkg'           # C:\Users\owner, C:\Windows, C:\ProgramData, ...

[workspace]                             # optional: roots for clean cases with a project marker
roots = ['F:\']

[[entry]]                               # the fake file tree; parent folders are implied
path        = 'C:\Users\owner\AppData\Local\zig\o\4c2f9a\main.obj'
size        = 4096                      # optional (bytes, default 4096)
age_days    = 365                       # optional (default 365)
folder      = true                      # optional: an empty folder; list it before its files
git_ignored = true                      # optional: git ignores it (a '.git' folder entry
                                        # makes its parent a work tree)

[[expect]]
kind     = "clean"                      # clean | attribution | relocate | compress | kb
case     = "global-cache"               # the local case id
match    = ['C:\Users\owner\AppData\Local\zig\o\4c2f9a']   # each must be selected
no_match = ['C:\Users\owner\AppData\Local\zig\o']          # none may be selected
```

What "selected" means per kind:

| Kind | Selected |
|---|---|
| clean, delete or quarantine | a candidate of the real `clean::RuleEngine` for the case, with every clean case of the pack evaluated together, SafeRemove's deny-list for the reference owner and no process running; `scan_artifacts` over `[workspace]` for a case with a project marker |
| clean, native or handler | an entry its targets measure |
| attribution, relocate | a path one of its locations names, component by component |
| compress | a path inside a folder its paths resolve to on the tree, not excluded |
| kb | an image name the entry is for, any case (`match = ['ArmouryCrate.Service.exe']`) |

A folder without files holds nothing, so the engine never proposes it: give it a file. Every
case that removes anything needs `no_match` entries for what must survive next to it (installed
libraries next to build trees, profiles next to caches).

## Adding a case

1. Find the pack of the tool, or create `data/packs/<tool>/pack.toml` (next section).
2. Back it: add the documentation URL, or what you observed on the reference machine, to
   `sources`, and cite it in a comment above the case.
3. Add the case to the kind's file. For cleaning, prefer in this order: the tool's own command
   (`native`), a Windows cleanup handler (`handler`, always for anything under `%WINDIR%`),
   then `delete` of entries the tool regenerates, with `min_age_days` and `not_in_use`.
   Default to `tier = "ask"`; `auto` only for caches whose loss costs nothing but a rebuild.
   Never target the owner's data: sessions, generated images, models, profiles, site data,
   installed packages and settings are attribution with `manual-review` (or a relocation)
   only.
4. Add an attribution area for the folder, pointing at the case.
5. Add or extend a fixture in `tests/` with `match` and `no_match` for the case, using the
   reference machine's real folder names.
6. Bump the pack's `version`, then run `ctest --preset debug -L packs` (or
   `myclean packs validate data\packs\<pack>`).

## Adding a pack

Create the folder with `pack.toml` (id = folder name, `version = "1.0.0"`, `min_myclean` = the
current MyClean version), at least one content file, an `applies_when` that tells whether the
tool is on the machine, and at least one fixture. Run the `packs` tests: the new pack is picked
up automatically.

## Shipped packs

Every rule id is `<pack>/<case>` (the former `windows.temp.user` is `windows/temp-user`, `pkg.pnpm.store` is
`node/pnpm-store`, ...): plans and logs stored before keep the old ids as history; new scans use
the new ones.

| Pack | Cases |
|---|---|
| `adobe` | attribution (review): app data and caches, media cache; Camera Raw profiles and models (uninstall); kb: Creative Cloud and Acrobat processes |
| `ai-tools` | attribution (review): Codex sessions and images, Claude Code transcripts, assistant homes, Ollama, LM Studio, Hugging Face and local AI models, Nexa Hyperlink, Cortex memory, Claude Code versions; relocate: OLLAMA_MODELS, HF_HOME; kb: Claude Code, Codex. Never cleaned |
| `android` | clean: SDK manager leftovers, tool caches; attribution: SDK, user home, system images and emulator devices (review); kb: emulator processes |
| `asus` | kb: Armoury Crate, Aura, ASUS services, A-Volute (Nahimic, Sonic Studio) |
| `browsers` | clean: HTTP, code and GPU caches of Chrome, Edge, Brave, Firefox; attribution: profiles and service worker caches (review); kb: Edge, WebView2 |
| `chrome` | clean: per-profile WebGPU (Dawn) caches; attribution: on-device AI model (review); kb: chrome.exe |
| `cpp` | clean: build output of inactive vcxproj and CMake projects |
| `databases` | attribution (uninstall): MongoDB and PostgreSQL servers, pgAdmin 4 |
| `creative-apps` | clean: Reallusion Hub downloads, Autodesk installer leftovers (ask); attribution: Reallusion, ZBrush, Marvelous Designer and Gaea content and autosaves (review), ArtEngine (uninstall); kb: Autodesk, FlexNet, Maxon, Reallusion processes |
| `dev-tools` | clean: Trivy's Java database; attribution (review/uninstall): source projects, SDKs and toolchains, ~/.cache; kb: Git |
| `device-software` | attribution: Corsair iCUE data (uninstall); kb: Realtek, Intel, Epson, TC-Helicon, Apple Bonjour |
| `dotnet` | clean: bin/obj of inactive .sln, .slnx and .csproj code bases, NuGet locals (`dotnet nuget locals`); attribution: NuGet packages (relocate) and caches |
| `electron-apps` | clean: caches and crash reports of Discord, Slack, Teams classic and new Teams, electron-updater downloads (`%LOCALAPPDATA%\*-updater`); attribution; kb: Discord, Teams |
| `flutter` | clean: .dart_tool and build of inactive Flutter projects |
| `games` | clean: Steam web and download caches, Epic launcher web cache, Unreal games' logs, crash reports and shader byte code; attribution: Steam, Epic and other launchers' games, Epic launcher data (uninstall); kb: Steam |
| `go` | clean: build and module caches (`go clean`); attribution: build and module caches (relocate) |
| `gpu` | clean: NVIDIA, AMD and DirectX shader caches, driver packages the NVIDIA app downloaded; attribution: driver installer leftovers (review), NVIDIA AI models (uninstall); kb: AMD and NVIDIA driver processes |
| `java-gradle` | clean: build output of inactive Gradle and Maven projects, unused Gradle versions; attribution: Gradle home (relocate), Maven repository (review); kb: Temurin java.exe |
| `jetbrains` | clean: IDE caches, local folders of old IDE versions; attribution: settings (review), IDEs (uninstall) |
| `node` | clean: node_modules and build output of inactive JavaScript projects, npm/pnpm/Yarn caches (their own prune commands), old Electron downloads and Bun cache entries; attribution: npm cache (relocate), global packages (review); kb: node.exe |
| `playwright` | clean: test browsers; attribution: browsers, MCP profiles (review) |
| `python` | clean: caches and build output of inactive Python projects, pip's uninstall leftovers ('~' folders in site-packages, on any fixed volume), pip and uv caches (their own commands); attribution: pip cache (relocate), uv, Python and Conda installs (uninstall); kb: python.exe |
| `python-store` | clean: pip cache of Store Pythons; attribution: installs (uninstall), user packages (review) |
| `rust` | clean: Cargo crate cache, sources and git checkouts, target of Cargo projects inactive 7 days, incremental caches no build wrote for 7 days (active projects too); compress: toolchains (LZX); relocate: RUSTUP_HOME; attribution: Cargo home (relocate), ort's ONNX Runtime downloads (review) |
| `security-network` | attribution: Kaspersky data; kb: Kaspersky, OpenVPN, WireGuard, AnyDesk, ExitLag |
| `unity` | clean: Library, Temp, Obj and Logs of inactive Unity projects (ask); attribution: editors (uninstall), package and Asset Store cache (review) |
| `unreal` | clean: build output, derived data, logs, crashes, cook and stage output and editor web caches of inactive Unreal projects, binaries as a case of their own (ask); attribution: engines (uninstall), shared DDC (relocate), per-user data, Vault cache and Quixel Bridge (Megascans) library (review) |
| `uzpainter` | clean: verification runs' pytest temporary folders, cache (ask); attribution (review): app data |
| `vcpkg` | clean: build trees, packages and downloads of the usual roots and of clones in the workspaces, binary cache archives; attribution (installed libraries: uninstall; roots: review) |
| `visual-studio` | clean: component (MEF) cache, the installer's package payloads, IntelliSense, browse-database, Copilot and search caches inside .vs of solutions inactive 7 days (also outside git), .vs state of inactive solutions; attribution: installations (uninstall), per-user data (review); kb: vctip |
| `vscode` | clean: old version data, extension packages, stale workspace storage, Chromium caches, webview resource cache (ask); attribution: web storage, extensions and Cursor data (review); kb: Code, Copilot runtime |
| `windows` | clean: temp folders, crash dumps, error reports, icon cache, PowerToys update installers, the Windows cleanup handlers, DISM component cleanup; attribution: system files and folders, programs, the owner's folders, MyClean's quarantine; kb: Windows, PowerToys, OneDrive and other Microsoft processes |
| `windows-setup-logs` | clean: the Setup Log Files and Windows Upgrade Log Files handlers; attribution: Panther (the upgrade-log handler), setup logs |
| `wsl-docker` | relocate: WSL 2 distributions (`wsl --manage --move`), WSL 1 distributions (`wsl-import`); clean: `docker system prune`, `wsl tmp prune` (entries of a WSL 1 distribution's /tmp older than 7 days, deleted from inside Linux); attribution: distribution disks, WSL 1 file systems and their /tmp, Docker Desktop disk (also in a custom location, and copies left by a move); kb: WSL and Docker processes |
| `zig` | clean: global cache entries, the `.zig-cache` of Zig projects inactive 7 days; attribution: global cache, project caches |