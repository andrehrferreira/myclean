# Packs Specification

## ADDED Requirements

### Requirement: Pack Layout and Manifest
Cleaning and optimization knowledge SHALL be shipped as packs, one folder per pack under `data/packs/<pack-id>/`, each with a `pack.toml` manifest (id equal to the folder name, semver version, title, description, sources, minimum MyClean version, optional applies_when) and at least one content file of a known kind: `clean.toml`, `attribution.toml`, `relocate.toml`, `compress.toml`, `kb.json` or `classes.toml`. The existing kinds MUST reuse the existing schemas (CleaningRule, attribution Area, KbEntry, ClassMatch).

#### Scenario: A valid pack loads
Given a folder `data/packs/zig` with a valid `pack.toml` whose id is "zig" and a valid `clean.toml`
When the packs are loaded
Then the pack is loaded with its manifest and its clean cases

#### Scenario: An unknown file rejects the pack
Given a pack folder that also holds `clean.tml`
When the packs are loaded
Then the pack is rejected with an error naming that file

### Requirement: Whole-Pack Validation
A pack SHALL be accepted or rejected whole. A rejected pack MUST add exactly one error that names the pack, the file, the case and the field, and every other pack MUST still load. A pack whose `min_myclean` is newer than the running MyClean MUST be refused the same way.

#### Scenario: A schema error in one pack
Given packs "alpha", "broken" and "gamma", where "broken" has a clean rule without a tier
When the packs are loaded
Then "alpha" and "gamma" are loaded
And one error reads like "pack 'broken': ...clean.toml: rule #1 'cache': field 'tier' is missing"

#### Scenario: A pack for a newer MyClean
Given MyClean 0.1.0 and a pack with `min_myclean = "0.2.0"`
When the packs are loaded
Then the pack is refused with "needs MyClean 0.2.0 or newer; this is 0.1.0"

### Requirement: Namespaced Case Ids
Every case id MUST be `[a-z0-9._-]`, unique within its file, and SHALL be exposed as `<pack>/<case>`. An attribution area's clean-rule reference MUST name a clean case of the same pack and SHALL be namespaced; a relocate reference naming a recipe of the same pack SHALL be namespaced, any other relocate reference SHALL be kept as a built-in relocation target, and a reference into another pack MUST be rejected.

#### Scenario: Ids and refs are namespaced
Given pack "zig" with clean case "global-cache" and an area whose reclaim ref is "global-cache"
When the pack is loaded
Then the clean case id is "zig/global-cache" and the area's ref is "zig/global-cache"

#### Scenario: A dangling ref
Given an area whose clean-rule ref names no clean case of its pack
When the pack is loaded
Then the pack is rejected naming the area and the field 'reclaim.ref'

### Requirement: Precedence, Overrides and Disabling
Owner packs in `%ProgramData%\MyClean\packs` SHALL load before built-in packs, and an owner pack SHALL shadow the built-in pack with the same id. A rejected owner pack MUST NOT shadow the built-in pack. Pack ids listed in `disabled` of the owner's `packs.toml` MUST NOT load from either place. A missing owner folder SHALL NOT be an error.

#### Scenario: An owner pack overrides a built-in pack
Given a built-in pack "alpha" and an owner pack "alpha"
When the packs are loaded
Then the owner's "alpha" is loaded and the built-in "alpha" is reported as shadowed

#### Scenario: A disabled pack
Given the owner's packs.toml holds `disabled = ["alpha"]`
When the packs are loaded
Then "alpha" is reported as disabled and contributes nothing

### Requirement: Conflict Detection
Every case SHALL claim what it acts on (clean targets with their project marker, native commands, cleanup handlers, attribution paths, relocation settings, compression paths, knowledge-base image and path, class criteria), compared case-insensitively. Two cases of one pack claiming the same thing MUST reject that pack; a pack claiming what an already loaded pack claims MUST be rejected whole with an error naming both cases.

#### Scenario: Two packs claim the same clean target
Given built-in packs "alpha" and "beta" that both target `%TEMP%\shared\*`
When the packs are loaded
Then "alpha" is loaded and "beta" is rejected naming "beta/cache" and "alpha/cache"

#### Scenario: The owner wins a conflict
Given a built-in pack and an owner pack that claim the same clean target
When the packs are loaded
Then the owner pack is loaded and the built-in pack is rejected

### Requirement: Applicability
A pack's applies_when SHALL hold when every `all` condition holds and, when `any` is given, at least one `any` condition holds. Conditions MUST be evaluated through interfaces: installed programs and Appx packages (regular expression), path existence after variable expansion with wildcards, a running process image, and a minimum Windows build. Only the packs that apply SHALL contribute to the registry built for this machine.

#### Scenario: A pack for a tool that is not installed
Given pack "ollama" applies when an installed app matches "^Ollama"
When no installed app matches
Then the registry for this machine holds none of its cases

#### Scenario: A path condition with an unset variable
Given a condition `path_exists = '%OLLAMA_MODELS%\blobs'`
When OLLAMA_MODELS is not set
Then the condition does not hold

### Requirement: Registry for the Engines
The registry SHALL expose the content of the packs in effect per kind, in load order, in the types the engines consume: a list of CleaningRule, attribution areas, relocation recipes, compression targets, knowledge-base entries and a ClassSet whose matches keep pack order.

#### Scenario: Merged views
Given loaded packs "zig" and "vscode" that apply
When the registry is built
Then its clean rules are those of "zig" followed by those of "vscode"

### Requirement: Relocation Recipes
A relocation recipe SHALL name where the content usually is, where its current location is read (an environment variable, a key of a tool's configuration file, a registry value whose wildcard key names instances, or a command), how the new location is set (the same setting, or a command with `{target}` and optionally `{name}`), whether the content moves or regenerates, the processes that must not run, and its undo: restore the previous value, run the command back to the previous folder, or none with a note.

#### Scenario: A command recipe without its target placeholder
Given a recipe with `target_kind = "command"` and a command without `{target}`
When relocate.toml is parsed
Then it is rejected naming the field 'command'

#### Scenario: Undo by restore for a command
Given a recipe with `target_kind = "command"` and `undo = "restore"`
When relocate.toml is parsed
Then it is rejected naming the field 'undo'

### Requirement: Compression Targets
A compression target SHALL name folder patterns with a real drive or variable, a WOF algorithm hint (xpress4k, xpress8k, xpress16k or lzx) and optional names excluded inside.

#### Scenario: NTFS compression is not a hint
Given a compression target with `algorithm = "lznt1"`
When compress.toml is parsed
Then it is rejected naming the field 'algorithm'

### Requirement: Per-Pack Fixture Tests
A pack MAY ship fixtures in `tests/*.toml` describing a fake file tree, environment variables and workspace roots, and, per case, paths that MUST be selected and paths that MUST NOT. Clean cases SHALL be checked against the real cleaning rule engine. ctest SHALL run one test per pack folder found under `data/packs` without any build change, and every shipped pack MUST have at least one fixture and pass it.

#### Scenario: A new pack is tested automatically
Given a new folder `data/packs/newtool` with a fixture
When ctest runs the packs label
Then a test named for "newtool" loads the pack and runs its fixture

#### Scenario: An unmet expectation
Given a fixture expecting a clean case to select a path the engine does not propose
When the fixture runs
Then the test fails naming the fixture, the expectation, the case and the path

### Requirement: Owner Data Is Never Cleaned by Packs
Cases covering the owner's work or chosen downloads (assistant sessions, generated images, local AI models, browser profiles and site data, installed packages) MUST be attribution with manual review or a relocation recipe, never a clean case.

#### Scenario: The ai-tools pack
Given the shipped ai-tools pack
When it is loaded
Then it has no clean case and every area is manual review or relocation
