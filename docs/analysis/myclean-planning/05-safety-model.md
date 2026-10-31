# 05 — Safety model

A cleaner that deletes the wrong thing once is never trusted again. Every
destructive path in MyClean goes through the rules below. Every one of the
owner's prompts repeats the same constraint ("do not delete anything until I
say so").

## Findings

### F-030 — Freed space and undo pull in opposite directions; choose per tier
- **Evidence:** Moving a file to the Recycle Bin or to a quarantine folder on
  the same volume is instant but frees nothing. Moving it to another volume
  frees the space but costs a full copy.
- **Impact:**
  - **`auto` tier** (regenerable caches, build outputs, temp): permanent
    delete. The undo is "it regenerates". Each run's manifest (path, size,
    rule) is still logged.
  - **`ask` tier:** **quarantine**. By default the items are renamed into
    `<volume>\.myclean-quarantine\<run-id>\` (an instant same-volume move) and
    purged after 7 days. When the volume is critically low (< 10 % free) they
    go to the configured quarantine volume instead (proposed: L: "Backup 2"),
    which frees the space immediately. Restore puts every item back on its
    original path from the manifest.
  - **`report` tier**, when the owner explicitly deletes something from a
    report view (a duplicate, an old download): Recycle Bin, through
    `IFileOperation` with `FOFX_RECYCLEONDELETE`, so Windows' own undo
    applies.
- **Confidence:** high.

### F-031 — Never follow reparse points or hydrate cloud files
- **Evidence:** pnpm's `node_modules` are made of junctions and symlinks.
  OneDrive files can be cloud-only placeholders. Recursive delete through a
  junction destroys the *target*, and reading a placeholder triggers a
  download.
- **Impact:** The scanner and the delete primitive open entries with
  `FILE_FLAG_OPEN_REPARSE_POINT`, never descend into reparse points, and
  remove a link as a link (`RemoveDirectoryW` on the junction itself). Files
  carrying `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, `RECALL_ON_OPEN` or
  `OFFLINE` are skipped, and sizes are reported as size-on-disk.
- **Confidence:** high.

### F-032 — One delete function, one deny-list, no exceptions
- **Evidence:** Rule files are data, and data can be wrong.
- **Impact:** All removal goes through a single `SafeRemove` primitive in
  `myclean-core`. It refuses, regardless of which rule asked, any path that is:
  a volume root; under `%WINDIR%` (except subpaths that are explicitly
  allow-listed, such as `Temp`); `Program Files*`; any `.git` folder or
  anything inside one; the user's Documents, Desktop, Pictures, Videos or
  Music (these are `report` only); another user's profile; or MyClean's own
  data. It also refuses any item that is not part of a plan MyClean itself
  produced (F-033). Unit tests cover every deny-list entry.
- **Confidence:** high.

### F-033 — The privileged service must not become a "delete anything" endpoint
- **Evidence:** The service runs as SYSTEM so it can touch system caches and
  assign jobs to elevated processes. Its IPC pipe is therefore an elevation
  boundary.
- **Impact:**
  - The pipe's DACL allows only SYSTEM and the logged-in owner's SID.
  - The service accepts no paths and no commands from clients. It only takes
    "run scan *rule-set*" and "execute plan *id*, items *[ids]*" for plans it
    created and stored itself. Every item is re-validated against the
    deny-list and the filesystem right before acting (to defeat
    time-of-check/time-of-use swaps).
  - **Per-user cleaning** (the user's temp, browser caches, package-manager
    commands) runs in the tray process with the user's own token, never as
    SYSTEM. The service only runs the built-in system rules.
  - Rule and config files under `%ProgramData%\MyClean` are writable by
    Administrators only. User rule overrides can only target paths that the
    user can already delete.
- **Confidence:** high.

### F-034 — AI clients may propose; only the owner approves, out of band
- **Evidence:** Prompts 1–5 explicitly say "do not close/delete anything on
  your own". A text channel to an AI is not a safe place for consent: a
  model can be wrong, or a web page can inject instructions into it.
- **Impact:** Through the MCP interface (see
  [06-assistant-prompts.md](06-assistant-prompts.md)) an AI client can read
  everything and **create a plan**, but cannot execute one. Executing a plan
  needs an approval click in the MyClean tray UI, which shows the items, the
  total size and the tier. The same applies to disabling a startup item or
  killing a process. Every action, with who proposed it and who approved it,
  goes to an append-only audit table.
- **Confidence:** high.
