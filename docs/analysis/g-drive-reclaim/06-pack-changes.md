# 06 Knowledge pack changes (proposed, not applied)

Nothing in `data/packs` was edited by this analysis. Every change below is written so it can
be pasted, then checked with `myclean packs validate data\packs` and `ctest -L packs`.

Owner directive: MyClean cleans on its own what is 100 % safe (pure caches the tool rebuilds),
never owner data. Every auto case below has `not_in_use` and a 7-day guard.

| # | Pack / case | Kind | Tier | GiB on G: | Needs code? |
|---|---|---|:-:|---:|---|
| A | `visual-studio/intellisense-cache`, `slnx-intellisense-cache` | new clean cases | auto | 11.07 (44.84 with E) | no (E for the rest) |
| B | `unreal/project-artifacts`: + `Saved\Cooked`, `Saved\StagedBuilds`, `Saved\webcache_*` | wider targets | ask | 0.29 (+6.11 with E) | no |
| C | `unreal/megascans-library` | new attribution area | — | 26.96 explained | no |
| D | `windows/installer-rollback` (`?:\Config.Msi`) | new attribution area | — | (unreadable, 0) | no |
| E | `require.outside_git` + spec amendment; applied to A and to Unreal output | engine field | auto / ask | +33.77 auto, +25.92 ask (+6.11 of B) | **yes** (08) |

## Drive letters

Clean targets cannot use `?:\`: `RuleEngine` resolves an absolute target from its root path
(`walk.cpp`, `resolve(tree, pattern.root_path(), ...)`), so `?:\X` is looked up as the literal
folder `?:\` and matches nothing, and `packs::parse` checks `?:` only for `path_exists`,
compression paths and relocation locations, so such a target is not even rejected. Attribution
paths do take `?:\` (any drive) and `*` within a component. **No proposal here needs a drive
letter:** the clean cases are project-relative (`project = { marker = ... }`, found under the
`data/workspaces.toml` roots, which include G:), and the two areas use `?:\`. A G:-specific
clean target (`'G:\UE\Megascans Library\...'`) would be the only way to clean a fixed folder on
G:, and none is proposed.

## A. `visual-studio/intellisense-cache` (new, auto)

Why it is safe: these are Visual Studio's code-intelligence caches. VS rebuilds each one in
the background when the solution is opened, without a build; losing them costs a few minutes of
indexing (the same trade as the shader caches, which the spec made auto). Microsoft documents the
browse database and the IntelliSense precompiled-header (ipch) cache as caches with a
configurable "fallback location" and a size quota (Options > Text Editor > C/C++ > Advanced).
The per-user state next to them (`.suo`, `DocumentLayout*.json`, `fileList.bin`,
`*.projects.v*.bin`, `DesignTimeBuild`) is never a target: `.vs` as a whole stays the ask-tier
`sln-state` case. The SQLite journals (`-wal`, `-shm`) go with their database, because a stale
journal next to a fresh database would be replayed into it.

`data/packs/visual-studio/clean.toml`, after `slnx-state`:

```toml
# Visual Studio's code-intelligence caches inside a solution's .vs folder (sources: pack.toml):
# the C++ IntelliSense precompiled-header cache (v17\ipch), the browse databases (Browse.VC.db,
# Solution.VC.db, with their SQLite -wal/-shm journals, which must go with them), Copilot's code
# index (CopilotIndices\<VS version>\CodeChunks.db, SemanticSymbols.db) and the Find in Files
# index (FileContentIndex\*.vsidx). Visual Studio rebuilds each in the background when the
# solution opens; no build is needed. The per-user state beside them (.suo, DocumentLayout.json,
# fileList.bin, *.projects.v*.bin, DesignTimeBuild) is never a target; .vs as a whole is the
# ask-tier sln-state case. Observed on reference machine: 15 .vs folders on G:, 45.06 GiB, of
# which 44.74 GiB are these caches (G:\ToS\_Backups\NetworkSample\.vs\NetworkSample: v17\ipch
# 9.89 GiB, v17\Browse.VC.db 2.32 GiB, CopilotIndices\17.14.794.21998 4.22 GiB). Artifact rules
# (cleaning spec, "Developer Artifact Qualification"): ignored by git, unused, and auto waits 7
# days for the project and for the cache itself.
[[rule]]
id       = "intellisense-cache"
category = "dev-artifacts"
tier     = "auto"
scope    = "user"
strategy = "delete"
project  = { marker = "*.sln" }
targets  = ['.vs\*\v1*\ipch', '.vs\*\v1*\Browse.VC.db', '.vs\*\v1*\Solution.VC.db',
            '.vs\*\v1*\*.VC.db-wal', '.vs\*\v1*\*.VC.db-shm', '.vs\*\CopilotIndices',
            '.vs\*\FileContentIndex']
require  = { git_ignored = true, not_in_use = true, inactive_days = 7, min_age_days = 7 }

[[rule]]
id       = "slnx-intellisense-cache"
category = "dev-artifacts"
tier     = "auto"
scope    = "user"
strategy = "delete"
project  = { marker = "*.slnx" }
targets  = ['.vs\*\v1*\ipch', '.vs\*\v1*\Browse.VC.db', '.vs\*\v1*\Solution.VC.db',
            '.vs\*\v1*\*.VC.db-wal', '.vs\*\v1*\*.VC.db-shm', '.vs\*\CopilotIndices',
            '.vs\*\FileContentIndex']
require  = { git_ignored = true, not_in_use = true, inactive_days = 7, min_age_days = 7 }
```

It passes `ArtifactRulesRequireEveryQualification` as it stands (git-ignored, not in use,
`inactive_days` set, auto with 7 days). The targets differ from `sln-state`'s `.vs`, so the pack
has no conflicting claim. `git check-ignore` reports the nested paths as ignored through the
`.vs` pattern (checked: `2025\Client\.vs\Client\v17\ipch` → `Client/.gitignore:7:.vs`).

**On G:** 11.07 GiB (`_Backups\Backup - ToS3\2025\Client\.vs` 10.44, `Server - LastCSharp`
0.35, `2025\Server` 0.12, `ServerCSharp1.0` 0.04, `GenericServer\Server` 0.06,
`Backup - ToS1\Server3` 0.01). With E: +33.77 (`NetworkSample` 17.26, `NoSafePlaces\Client`
3.40, `UnderTheShadow` 3.39, `ToS\Client` 3.35, `UE - Projects\PlayerController` 3.24,
`ClientNetworkSample` 3.02, `ToS\Client\…\libsodium` 0.05 stays out: its git file is broken).

`data/packs/visual-studio/pack.toml`: `version = "1.2.0"` (new cases); `description` gains "the
IntelliSense, browse-database, Copilot and search caches inside .vs (cleaned on its own)";
`sources` gain:

```toml
  "https://learn.microsoft.com/visualstudio/ide/reference/options-text-editor-c-cpp-advanced (the C++ browse database and the IntelliSense precompiled-header cache (ipch) are caches with a fallback location and a size quota, kept in the solution's .vs folder by default)",
  "observed on reference machine: G:\\ 15 .vs folders, 45.06 GiB: v17\\ipch (AutoPCH and *.ipch) 17.76, Browse.VC.db and Solution.VC.db 12.65, CopilotIndices\\17.14.794.21998 (CodeChunks.db, SemanticSymbols.db) 12.55, FileContentIndex\\*.vsidx 1.78, per-user state (.suo, DocumentLayout.json, fileList.bin) 0.32",
```

`data/packs/visual-studio/tests/intellisense.toml` (new fixture, the reference machine's names):

```toml
# The code-intelligence caches of three solutions on G:: the Client backup and a server, both in
# git work trees that ignore .vs (their caches go, the per-user state stays), and the live client
# outside git (kept: nothing vouches for it).

[workspace]
roots = ['G:\']

[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\.git'
folder = true
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\Client.sln'
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\Source\Client\Client.Build.cs'
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs'
folder = true
git_ignored = true
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\ipch'
folder = true
git_ignored = true
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\ipch\109baed67d4092a.ipch'
size = 268435456
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\Browse.VC.db'
size = 2427936768
git_ignored = true
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\Solution.VC.db'
size = 402182144
git_ignored = true
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\.suo'
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\DocumentLayout.json'
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\FileContentIndex'
folder = true
git_ignored = true
[[entry]]
path = 'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\FileContentIndex\38ef5d94-93dd-4be6-9dff-4d93a83bffc2.vsidx'

[[entry]]
path = 'G:\GenericServer\Server\.git'
folder = true
[[entry]]
path = 'G:\GenericServer\Server\Server.sln'
[[entry]]
path = 'G:\GenericServer\Server\Program.cs'
[[entry]]
path = 'G:\GenericServer\Server\.vs'
folder = true
git_ignored = true
[[entry]]
path = 'G:\GenericServer\Server\.vs\Server\CopilotIndices'
folder = true
git_ignored = true
[[entry]]
path = 'G:\GenericServer\Server\.vs\Server\CopilotIndices\17.14.794.21998\CodeChunks.db'
[[entry]]
path = 'G:\GenericServer\Server\.vs\Server\v17\Browse.VC.db'
git_ignored = true
[[entry]]
path = 'G:\GenericServer\Server\.vs\Server\v17\Browse.VC.db-wal'
git_ignored = true
[[entry]]
path = 'G:\GenericServer\Server\.vs\Server\v17\Browse.VC.db-shm'
git_ignored = true
[[entry]]
path = 'G:\GenericServer\Server\.vs\Server\v17\Browse.VC.opendb'
[[entry]]
path = 'G:\GenericServer\Server\.vs\Server\DesignTimeBuild\.dtbcache.v2'
[[entry]]
path = 'G:\GenericServer\Server\.vs\ProjectEvaluation\server.projects.v9.bin'

[[entry]]
path = 'G:\ToS\Client\Client.sln'
[[entry]]
path = 'G:\ToS\Client\.vs\Client\v17\Browse.VC.db'
[[entry]]
path = 'G:\ToS\Client\.vs\Client\CopilotIndices\17.14.794.21998\SemanticSymbols.db'

[[expect]]
kind     = "clean"
case     = "intellisense-cache"
match    = ['G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\ipch',
            'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\Browse.VC.db',
            'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\Solution.VC.db',
            'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\FileContentIndex',
            'G:\GenericServer\Server\.vs\Server\CopilotIndices',
            'G:\GenericServer\Server\.vs\Server\v17\Browse.VC.db',
            'G:\GenericServer\Server\.vs\Server\v17\Browse.VC.db-wal',
            'G:\GenericServer\Server\.vs\Server\v17\Browse.VC.db-shm']
no_match = ['G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs',
            'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17',
            'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\.suo',
            'G:\ToS\_Backups\Backup - ToS3\2025\Client\.vs\Client\v17\DocumentLayout.json',
            'G:\ToS\_Backups\Backup - ToS3\2025\Client\Source',
            'G:\GenericServer\Server\.vs\Server\DesignTimeBuild',
            'G:\GenericServer\Server\.vs\ProjectEvaluation',
            'G:\GenericServer\Server\.vs\Server\v17\Browse.VC.opendb',
            'G:\ToS\Client\.vs\Client\v17\Browse.VC.db',
            'G:\ToS\Client\.vs\Client\CopilotIndices']
```

(After E, the `G:\ToS\Client` entries move from `no_match` to `match`, and a `no_match` stays
for a `.vs` cache inside a git work tree that does *not* ignore `.vs`.)

## B. `unreal/project-artifacts`: cook, stage and editor web cache (ask, wider)

`Saved\Cooked` and `Saved\StagedBuilds` are the cook and stage output of "Package Project" and
the Project Launcher; `Saved\webcache_<n>` is the editor's embedded Chromium (CEF) cache, one
folder per CEF branch (4430, 6613). All three are written again by the next cook, stage or
editor start. `Saved` itself and `Saved\Config`, `Autosaves`, `Collections`, `SourceControl`
stay out, as the spec requires.

`data/packs/unreal/clean.toml`, the rule becomes:

```toml
# Unreal: rebuilding Binaries and DerivedDataCache takes long, so it is "ask". Saved holds the
# project's config, so only its regenerated subfolders are targets, never Saved itself: Logs,
# Crashes, the cook and stage output (Saved\Cooked, Saved\StagedBuilds: written again by the
# next cook or package) and the editor's embedded Chromium cache (Saved\webcache_<CEF branch>).
# Observed on reference machine: G:\UE_Projects\ClothTest55\Saved\Cooked 1.99 GiB and
# StagedBuilds 1.71 GiB; Saved\webcache_4430 and webcache_6613 in 9 projects, 1.58 GiB.
[[rule]]
id       = "project-artifacts"
category = "dev-artifacts"
tier     = "ask"
scope    = "user"
strategy = "delete"
project  = { marker = "*.uproject" }
targets  = ["Intermediate", "DerivedDataCache", "Binaries", 'Saved\Logs', 'Saved\Crashes',
            'Saved\Cooked', 'Saved\StagedBuilds', 'Saved\webcache_*']
require  = { git_ignored = true, not_in_use = true, inactive_days = 14 }
```

`catalog_test` forbids only the target `Saved`; these pass. **On G:** +0.29 GiB now (the two
`webcache_4430` folders in the git backups, both ignored by `Saved` in `.gitignore`); with E
the rule also reaches the 29 projects outside git (`Cooked`/`StagedBuilds` 4.82, webcache 1.29).

Fixture, appended to `data/packs/unreal/tests/projects.toml` (same `F:\Unreal\ShooterDemo`
project, which is in git):

```toml
[[entry]]
path = 'F:\Unreal\ShooterDemo\Saved\Cooked'
folder = true
git_ignored = true
[[entry]]
path = 'F:\Unreal\ShooterDemo\Saved\Cooked\Windows\ShooterDemo\Content\Maps\Arena.umap'
[[entry]]
path = 'F:\Unreal\ShooterDemo\Saved\StagedBuilds'
folder = true
git_ignored = true
[[entry]]
path = 'F:\Unreal\ShooterDemo\Saved\StagedBuilds\Windows\ShooterDemo\Content\Paks\ShooterDemo-Windows.ucas'
[[entry]]
path = 'F:\Unreal\ShooterDemo\Saved\webcache_4430'
folder = true
git_ignored = true
[[entry]]
path = 'F:\Unreal\ShooterDemo\Saved\webcache_4430\Cache\data_0'
[[entry]]
path = 'F:\Unreal\ShooterDemo\Saved\Autosaves\Game\Maps\Arena_Auto1.umap'
[[entry]]
path = 'F:\Unreal\ShooterDemo\Saved\Collections\Favorites.collection'
```

and the existing `[[expect]]` for `project-artifacts` becomes:

```toml
[[expect]]
kind     = "clean"
case     = "project-artifacts"
match    = ['F:\Unreal\ShooterDemo\Intermediate', 'F:\Unreal\ShooterDemo\DerivedDataCache', 'F:\Unreal\ShooterDemo\Binaries',
            'F:\Unreal\ShooterDemo\Saved\Logs', 'F:\Unreal\ShooterDemo\Saved\Crashes', 'F:\Unreal\ShooterDemo\Saved\Cooked',
            'F:\Unreal\ShooterDemo\Saved\StagedBuilds', 'F:\Unreal\ShooterDemo\Saved\webcache_4430']
no_match = ['F:\Unreal\ShooterDemo\Saved', 'F:\Unreal\ShooterDemo\Saved\Config', 'F:\Unreal\ShooterDemo\Content',
            'F:\Unreal\ShooterDemo\Config', 'F:\Unreal\ShooterDemo\Saved\Autosaves', 'F:\Unreal\ShooterDemo\Saved\Collections']
```

## C. `unreal/megascans-library` (new attribution area)

`data/packs/unreal/attribution.toml`:

```toml
# Quixel Bridge's local library (the folder set in Bridge > Settings > Local Library Path):
# Downloaded\UAssets\<asset id>\Tier<n> holds the Unreal assets Bridge downloaded; adding one to
# a project copies it into the project's Content\Megascans. Observed on reference machine:
# G:\UE\Megascans Library, 26.96 GiB, every file of 1 MiB or more also in a project
# (G:\UnderTheShadow\Content\Megascans 25.73 GiB).
[[area]]
id       = "megascans-library"
paths    = ['%USERPROFILE%\Documents\Megascans Library', '?:\Megascans Library', '?:\*\Megascans Library']
category = "dev-caches"
label    = "Quixel Bridge (Megascans) local library"
reclaim  = { kind = "manual-review", note = "Assets a project already holds are copies here; Bridge adds them to the next project from this folder, and downloading them again depends on your Fab/Quixel account." }
```

`pack.toml`: `version = "1.1.0"` (with B, one MINOR bump covers both), `description` gains "the
Quixel Bridge (Megascans) library", `sources` gains
`"observed on reference machine: G:\\UE\\Megascans Library\\Downloaded\\UAssets\\<id>\\Tier0 (26.96 GiB), also in G:\\UnderTheShadow\\Content\\Megascans (Quixel Bridge's local library)"`.

Fixture, appended to `data/packs/unreal/tests/projects.toml`:

```toml
[[expect]]
kind     = "attribution"
case     = "megascans-library"
match    = ['G:\UE\Megascans Library', 'C:\Users\owner\Documents\Megascans Library']
no_match = ['G:\UE', 'G:\UnderTheShadow\Content\Megascans']
```

## D. `windows/installer-rollback` (new attribution area)

The map lists `G:\Config.Msi` as unreadable with no area. It is Windows Installer's rollback
folder (rollback scripts and the files an installation replaced; empty when no installation is
running or was interrupted).

`data/packs/windows/attribution.toml`, after `recycle-bin`:

```toml
# Windows Installer's rollback folder on each volume (sources: pack.toml): rollback scripts and
# the files an installation replaces, kept until it commits. Only administrators can open it.
[[area]]
id       = "installer-rollback"
paths    = ['?:\Config.Msi']
category = "system"
label    = "Windows Installer rollback files (needs admin to size)"
reclaim  = { kind = "none" }
```

`pack.toml`: PATCH bump (an area changes no selection); `sources` gains
`"https://learn.microsoft.com/windows/win32/msi/rollback-installation (the installer keeps rollback scripts and backed-up files until the installation commits)"`
and `"observed on reference machine: G:\\Config.Msi (hidden, access denied to the owner)"`.
Fixture (`data/packs/windows/tests/volume.toml`):

```toml
[[expect]]
kind     = "attribution"
case     = "installer-rollback"
match    = ['G:\Config.Msi', 'C:\Config.Msi']
no_match = ['G:\ToS']
```

## E. `require.outside_git` (code + spec; approval needed)

Today a project rule with `git_ignored = true` drops every candidate outside a git work tree
(`qualify()`: `not_in_git`). That is right for names like `build` or `dist`, which a project
may hold by hand. It is too strict for paths only a tool writes: `.vs\<sln>\v17\ipch` or an
Unreal project's `Intermediate` hold nothing a person authored, whether or not the project is in
git. On G: this is 59.7 GiB (33.77 VS caches, 25.92 Unreal output), all in projects inactive
for 6 to 16 months.

Proposed field, `require.outside_git = true`: inside a work tree nothing changes (git must
report the folder ignored and track nothing in it); outside any work tree the candidate is not
dropped for `not_in_git`, and every other guard still applies (`not_in_use`, `inactive_days`
over the project's files, `min_age_days`).

Spec delta (`.rulebook/tasks/phase2_free-c-drive/specs/cleaning/spec.md`, "Developer Artifact
Qualification"), one sentence after the first: "A rule whose targets are written only by a
tool (Visual Studio's `.vs` caches, an Unreal project's `Intermediate`, `DerivedDataCache`,
`Saved\Logs`, `Saved\Crashes`, `Saved\Cooked`, `Saved\StagedBuilds`, `Saved\webcache_*`) MAY set
`outside_git`, which accepts a project outside any git repository; inside a repository git must
still vouch for the folder." Plus a scenario: an Unreal project outside git, inactive 30 days,
its `Intermediate` is proposed (ask) and its `Binaries` and `Content` are not.

Then the data:

```toml
# visual-studio/clean.toml: both intellisense-cache cases
require  = { git_ignored = true, outside_git = true, not_in_use = true, inactive_days = 7, min_age_days = 7 }
```

```toml
# unreal/clean.toml: Binaries moves to its own case, because a project without Source needs
# them; everything else in project-artifacts is written only by the engine and its tools.
[[rule]]
id       = "project-artifacts"
category = "dev-artifacts"
tier     = "ask"
scope    = "user"
strategy = "delete"
project  = { marker = "*.uproject" }
targets  = ["Intermediate", "DerivedDataCache", 'Saved\Logs', 'Saved\Crashes', 'Saved\Cooked',
            'Saved\StagedBuilds', 'Saved\webcache_*']
require  = { git_ignored = true, outside_git = true, not_in_use = true, inactive_days = 14 }

# Binaries: rebuilt from Source; only when git vouches (a project without Source needs them).
[[rule]]
id       = "project-binaries"
category = "dev-artifacts"
tier     = "ask"
scope    = "user"
strategy = "delete"
project  = { marker = "*.uproject" }
targets  = ["Binaries"]
require  = { git_ignored = true, not_in_use = true, inactive_days = 14 }
```

Pack versions: `visual-studio` and `unreal` MINOR again (wider selection; no case id removed);
`min_myclean` raised to the first MyClean whose parser knows `outside_git`. Fixtures: the
`G:\ToS\Client` entries of A's fixture become `match`; `unreal/tests/projects.toml` gains a
project outside git (`G:\UnderTheShadow\UnderTheShadow.uproject`, `Intermediate\Build\Win64\...`,
`Binaries\Win64\UnrealEditor-UnderTheShadow.dll`, `Content\Megascans\...`) whose `Intermediate`
matches `project-artifacts` and whose `Binaries` and `Content` match nothing.

**On G: with E:** auto +33.77 (VS caches), ask +25.92 Unreal output +6.11 (B's new targets) =
32.03 GiB in projects outside git.

## Checked and left alone

- `rust/target`, `rust/stale-incremental`: already reach `G:\ToS\Server\target` (28.32) and the
  backup's `Benchmark\target` (0.52). Nothing to change.
- `node/artifacts`, `dotnet/*`, `cpp/cmake-artifacts`: select what they should (03).
- `unity/project-artifacts`: the only Unity project (`repo-recuperado`) has no `Library`.
- No rule for archives, backups, autosaves, the Megascans library or `MutableStreamedDataEditor`:
  owner data or not proven regenerable (07).
- No relocation or compression recipe: G: content is `.uasset`, `.zip`, `.rar`, textures and
  ZBrush files, already compressed; the cold-data move (scan-all) covers moving to an HDD.
