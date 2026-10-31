# 1. Default policy values for v1 (D2–D7)

**Status**: proposed
**Date**: 2026-10-31

## Context

docs/prd.md §10 listed owner decisions D2–D7. All are configurable defaults, reversible, inside the spec, so they are decided here with the recommended values; the owner can change any of them in settings.

## Decision

D2 quarantine overflow volume = L: (healthy HDD). D3 workspace roots = F:, W: (configurable list). D4 governor caps: build CPU 75 % / commit 96 GB, test CPU 75 % / commit 64 GB, game-mode build cap 40 %. D5 kill disabled by default for every class. D6 VirusTotal off until an API key is provided (hash-only). D7 sensor bridge source = HWiNFO shared memory when installed (post-v1).

## Alternatives Considered

- Ask the owner for each value before M0 — rejected: all are runtime settings with safe defaults

## Consequences

Specs and tests use these values as defaults; changing one is a config edit, not a code change.
