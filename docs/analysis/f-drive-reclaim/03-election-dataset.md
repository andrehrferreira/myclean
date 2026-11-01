# 03 The election-audit dataset (`F:\Node\auditoria-2turno-eleicao2026`, 560.97 GiB)

A Node.js project (`package.json`, git; last commit 2026-10-31 05:43, so active) that audits
the TSE's published voting-machine files of the 2026 election. Its `.gitignore` keeps
`data/raw/`, `data/parsed/` and `data/db/` out of git with the comment "large / reproducible
via scripts". Everything here is the owner's data: status 3 or –.

| GiB | Folder | What it is |
|---:|---|---|
| 123.77 | `data\raw\2t\urnas` | round 2: the zips extracted per section |
| 115.61 | `data\raw\2t\zip` | round 2: 27 TSE zips `bu_imgbu_logjez_rdv_vscmr_2026_2t_<UF>.zip` (SP 24.0, MG 11.7, RJ 9.0, …) |
| 106.28 | `data\raw\1t\urnas` | round 1: extracted |
| 88.54 | `data\raw\1t\zip` | round 1: zips |
| 21.67 | `data\raw\2022\2t\zip` | 2022 round 2 zips |
| 17.44 | `data\raw\2022\1t\zip` | 2022 round 1 zips |
| 7.15 | `data\raw\dadosabertos` | 294 zips of TSE open data |
| 18.98 | `data\db\leitura\eleicao2026-20261029T105000.sqlite` | the current read copy (`atual.json` points to it) |
| 17.94 + 1.01 | `data\db\eleicao2026.sqlite` + `-wal` | the primary database |
| 11.84 | `data\db\leitura\eleicao2026-20261029T092711.sqlite` | an older read copy, superseded |
| 30.08 | `data\parsed` | 1,966 JSONL files (27.66), CSV 0.96, TSV 0.37, `.bin` 0.71, … |
| 0.41 | `data\manifest` | logs, coverage |
| ~0.07 | `.git`, `src`, `docs`, `scripts`, `test` | sources |

## The zips and `urnas` hold the same files

`src/etapas.js`, step 3: "extracts each zip to `urnas/{uf}/{mun}/{zona}/{secao}/{hash}/` (CRC
checked)". The README: `raw/<t>t/zip` (the TSE zips) and `raw/<t>t/urnas/...` (extracted files);
`node src/coleta.js [--turno 1|2] [--uf …]` runs collection with resume: index, zips,
extraction, `aux.json`, check. The extracted tree adds one `aux.json` per section from the TSE
API and a `log.jez` missing from the SP zip that was fetched per section (README).

So one of the two is redundant while the other exists:

- **Delete the extracted `urnas` (230.05 GiB + ~10 GiB of NTFS slack, 01)**, keep the zips.
  `coleta.js` extracts again when needed. Cost: the analyses H08, H13, H19 and H20 read the logs
  of every machine directly from the extracted tree (README), so re-extraction (hours) comes
  before re-running them; `aux.json` is fetched again from the TSE.
- **Or delete the zips (204.15 GiB)**, keep the extracted tree. The zips' SHA-256 are in the
  collection registry, and the TSE publishes them; but the evidence chain (`docs/pericia`) may
  cite the zips themselves.

Which one is the owner's (and the audit's) call. Neither is a pack case: this is one person's
project, not knowledge about a tool.

## The superseded read copy (11.84 GiB)

`src/banco-leitura.js` publishes a new read copy per load and points `atual.json` at it; old
copies stay until `node src/banco-leitura.js --remover-antigas`, which deletes every copy that is
not current and not open by another process. `atual.json` names `…T105000`, so `…T092711` goes.
This is the project's own prune command, safe by its own design, but it is project-specific:
the owner runs it (07).

## 2022 zips (39.11 GiB)

Used for the 2022–2026 comparisons in the latest commits (2026-10-31: "sinal V2 do Sul e do RS
já aparecia em 2022"). Keep while that work goes on; the TSE republishes them.

## Compression (status 2)

The JSONL in `data\parsed` compress to 12–25 % in the samples (`seguranca-h25\1t-padrao-certs.jsonl`
12 %, `cetico\40-imgbu-1t.jsonl` 13 %, `integridade\2t\secoes.jsonl` 25 %, `1t\perfil-v3\sp.jsonl`
25 %). LZX on `data\parsed` would save ~22 GiB. The scripts read them unchanged (WOF
compression is transparent; a rewrite stores the file uncompressed until compressed again).
The compression analyzer skips files changed recently, so it applies once the project calms
down. The zips (already compressed), the SQLite databases (excluded by the analyzer) and the
`urnas` files (small, mostly signed binaries and `.jez` archives) are not worth it.

## Moving it

`src/config.js` fixes `DIR_DADOS` to `<repo>\data`; there is no setting to put the data
elsewhere, and MyClean never moves with junctions. Moving the whole project folder to another
drive (git and all) is the owner's option, not a MyClean recipe.

## How the workspace scanner treats it

The scanner finds `package.json` at depth 2 (rule `node/artifacts`) but no `node_modules`,
`dist`, `build`, `coverage` or `.cache` in it, so it never runs the inactivity walk over these
8 M entries. Its folder walk stops at depth 8 (`…\urnas\<uf>\<municipality>`), ~11,000 listings.
