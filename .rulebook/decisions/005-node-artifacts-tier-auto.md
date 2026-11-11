# 5. Node build artifacts are deleted automatically (tier auto)

**Status**: accepted
**Date**: 2026-11-01

## Context

The node pack's `artifacts` rule (data/packs/node/clean.toml) is tier `auto`. It selects `node_modules`, `.next`, `.nuxt`, `.turbo`, `.parcel-cache`, `.svelte-kit`, `dist`, `build`, `coverage` and `.cache` of a package.json project. The cleaning executor deletes auto-tier dev artifacts outright instead of quarantining them. The P0 audit (docs/analysis/p0-audit/02-gaps.md, correctness risk 9) found that no decision recorded this, and that an ignored release build in `dist` or `build` cannot be undone.

On 2026-11-01 the owner's rule was that everything 100 % safe is cleaned without approval (commit 888621f). For a dev artifact, the owner defined 100 % safe as an item that is all of these:

- regenerable;
- ignored by git;
- untracked;
- in a project inactive for 30 days;
- not in use.

## Decision

The rule stays tier `auto` and deletes without quarantine, but only when every guard holds:

- the project is a git work tree and `git check-ignore` reports the artifact ignored;
- git tracks nothing inside the artifact;
- the project has been inactive for at least 30 days (`inactive_days = 30`), counting from the newest of its last commit and every source file;
- no process has its current directory or an open file in the artifact or the project.

A folder the owner wants to keep must not match these guards. For example, the owner commits it, or keeps it outside an ignored path.

## Alternatives Considered

- Tier `ask`, or quarantine before deleting: rejected by the owner. These folders are large and rebuilt by the next install or build, so a 7-day quarantine would keep the space taken.

## Consequences

- PRD CLN-2 points here.
- A release build kept only in an ignored `dist` or `build` folder of an inactive project is deleted. It is gone for good.
