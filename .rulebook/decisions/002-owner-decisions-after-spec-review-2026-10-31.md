# 2. Owner decisions after spec review (2026-10-31)

**Status**: proposed
**Date**: 2026-10-31

## Context

Spec authoring surfaced product questions the docs did not settle; the owner answered them.

## Decision

1) UI technology: Win32 tray + Dear ImGui + ImPlot (D3D11). 2) SQLite migrations are additive-only; an older build opens a newer schema without migrating and ignores unknown tables/columns, so deploy.ps1 rollback never loses history. 3) Reverse DNS for network talkers is opt-in and off by default (NFR-SEC-3). 4) Governor: Allow once exempts a tree from the ladder until its root exits; Resume blocks re-suspension for 10 min. 5) UI, notifications and reports in English.

## Consequences

Specs updated: phase0 store (Additive-Only Migrations), phase3 governor (Owner Exemptions Survive Continued Pressure), phase5 security (reverse DNS opt-in); PRD UI-7 and D1 updated.
