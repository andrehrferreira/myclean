# 07 Decisions for the owner

Only the owner can make these. GiB are what each frees on F:. "MyClean" says what MyClean can do
once approved; otherwise the owner acts in the named tool.

| # | GiB | Decision | Action | MyClean |
|---:|---:|---|---|---|
| 1 | **998.95** | **Delete the old Docker Desktop disk copy** `F:\WSL` (`DockerDesktopWSL.ANTIGO\disk\docker_data.vhdx` 998.4, last written 2026-09-11, plus two unused `ext4.vhdx`). Docker runs from `D:\Database\DockerDesktopWSL` (02). | Start Docker Desktop and check your containers and volumes; then Shift+Delete `F:\WSL` (or `Remove-Item -LiteralPath F:\WSL -Recurse`). Optional: re-save Settings > Resources > Advanced > Disk image location so the legacy `settings.json` stops naming F:. | review only (new area `wsl-docker/docker-data-custom`) |
| 2 | **230.1** (+~10 NTFS) **or 204.2** | **Election dataset: keep either the TSE zips or their extracted copy**, not both (03). Deleting `urnas` also removes ~2 M folders and ~6 M files, which speeds up every scan of F:. | Either delete `F:\Node\auditoria-2turno-eleicao2026\data\raw\1t\urnas` and `…\2t\urnas` (re-extract with `node src/coleta.js --turno 1` / `--turno 2` before re-running H08, H13, H19, H20), or delete `…\raw\1t\zip` and `…\raw\2t\zip` (if the evidence chain does not need the zip files themselves) | — |
| 3 | 11.84 | **Superseded read copy of the audit database** `data\db\leitura\eleicao2026-20261029T092711.sqlite` | `node src/banco-leitura.js --remover-antigas` in the project (deletes copies that `atual.json` does not name and nobody has open) | — |
| 4 | 39.11 | **2022 TSE zips** (`data\raw\2022`) | Keep while the 2022 comparison is in use; delete afterwards (the TSE republishes them) | — |
| 5 | ~22 | **Compress `data\parsed`** (27.7 GiB of JSONL, 12–25 % in samples) | Once the project is quiet, approve the compression plan for the folder | `CompressFolder` LZX; undo: decompress |
| 6 | 2.94 / 2.3 | **MongoDB 8.0 server** (stopped; set to Automatic) | Not used: export `data`, `sc.exe delete MongoDB` (elevated), delete `F:\MongoDB`. Used: delete `bin\mongod.pdb` and `bin\mongos.pdb` (1.8), rotate `mongod.log` (0.5) | review only (new `databases` pack) |
| 7 | 0.66–0.77 | **Two pgAdmins** (`F:\pgAdmin 4` 9.9 and the one inside `F:\PostgreSQL\18`) | Settings > Apps > uninstall "pgAdmin 4 version 9.9", or remove pgAdmin from PostgreSQL's installer (Modify) | review only |
| 8 | ~3.8 | **Compress LLVM, Python313 and MongoDB's bin** (~1.7 + ~1.3 + ~0.8) | — | `CompressFolder` LZX; undo: decompress |
| 9 | 0.50 | **Recycle Bin of F:** (266 files, 2022 – 2026-10-30) | Empty it | approve `windows/recycle-bin` (ask) |
| 10 | — | **Shadow storage on F:** (not readable without admin) | Elevated: `vssadmin list shadowstorage /for=F:` and cap it if System Protection is on for F: | setting `shadow_storage` |

**For the coordinator, not the owner:** 55 MyClean build configurations of finished agents
(56.8 GiB in `F:\MyClean\build`, last built 18–29 h ago) can go when their work has merged
(04). The pip leftovers (3.79 GiB) need no decision: once `?:\` targets exist, MyClean deletes
them on its own.

If the owner does 1, 2 (the extracted copy) and 3, F: goes from 184.6 GiB free (9.9 %) to
~1,435 GiB free (77 %).
