# Proposal: phase2c_knowledge-packs

## Why
The owner wants every useful cleaning/optimization pattern captured as data and grown over updates. Today the knowledge is split across unrelated data files (rules/*.toml, attribution.toml, kb/*.json, classes.toml, drive-specs.json) with no unit of contribution, no applicability conditions and no per-pattern tests. A modular pack system makes adding a new ecosystem or case a data-only change with its own tests.

## What Changes
Knowledge packs: data/packs/<pack>/ with a pack.toml manifest (id, version, title, sources, min MyClean version, applies_when: app installed / path exists / OS build) and content files per case kind (clean rules, attribution, relocation recipes, compression targets, KB entries, process classes); a pack loader that discovers, validates (whole-pack rejection), namespaces ids, resolves conflicts, enables/disables packs and feeds the existing engines; owner override packs under %ProgramData%\MyClean\packs; per-pack fixture tests run by ctest automatically; `myclean packs list|validate`; docs/packs.md contribution guide; migration of the existing data files into packs; a first wave of new packs from the real C: map.

## Impact
- Affected specs: packs
- Affected code: src/core/packs, src/core/{rules,map,placement,compress,trust}, src/cli, data/packs, docs/packs.md
- Breaking change: NO
- User benefit: Adding cleaning/optimization knowledge becomes a data-only change with its own tests; MyClean grows a complete catalog over updates
