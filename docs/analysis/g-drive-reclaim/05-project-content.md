# 05 Project content and the owner's assets (kept)

**Answer.** 438.1 GiB of G: is content nobody but the owner can judge, and none of it is a
cleaning case: the Unreal projects' content and plugins (286.1 GiB: `NoSafePlaces` 198.4,
`UE_Projects` 61.8, `UnderTheShadow` 26.0), the ZBrush base library and brushes (89.6 GiB
extracted), the owner's own creatures and props (43.7 GiB), purchased models (13.7 GiB) and
small live trees (4.7 GiB). It is
all cold (newest 2026-04-17), so it can move to an HDD project by project if the owner wants
the NVMe space, but project folders work best on an SSD, so this analysis does not count it as
reclaimable.

| GiB | Folder | Notes |
|---:|---|---|
| 176.07 | `NoSafePlaces\Client\Content` | 60+ imported packs: REALISTIC_MATERIALS 33.65, JOBSITE 12.88, YI_ModularZombies 12.79, SCANS 10.44, MMC 10.05, Barnyard 7.42, ... Project last written 2025-08-11. `NoSafePlaces\Client` carries a `$movedMarker` file (2025-08-02): it was moved here from another place. |
| 45.04 | `UE_Projects\AssetNormalize\Content` | packs being normalized: CastleTown 7.69, Hivemind 4.86, CastleofEternalMist 4.85, ... (2025-08-25) |
| 25.75 | `UnderTheShadow\Content\Megascans` | the Megascans assets also in `G:\UE\Megascans Library` (04) |
| 19.46 | `NoSafePlaces\AssetsNormalization\Content` | SCHOOL, AutoRockCliffs (next to its `.rar`), SUBURBS, `__ExternalActors__` |
| 8.77 / 2.71 / 1.21 / 0.12 | `UE_Projects\ClothTest55`, `MutableSample`, `ClothTest`, `PlayerController` `Content` | tests and samples (ParagonAurora, MetaHumans, Mutable sample) |
| 3.78 | `UE_Projects\PlayerController\Plugins\GASPALS` and `GASP-ALS\Plugins\GASPALS` | the same plugin in two projects (1.89 each) |
| 2.87 | `NoSafePlaces\Client\Plugins` | DLSS, Streamline, NIS, ALS-Refactored: their `Binaries` (1.58) ship prebuilt and are not regenerable without the plugin source |
| 70.80 + 12.13 + 5.49 + 1.17 | `ToS\_Base\Bases ZBrush`, `Brushes`, `Meshs`, `SSP` (extracted part) | the owner's purchased sculpting library |
| 32.97 + 10.75 | `ToS\Assets - Custom\Creatures` and `Resources`, `Weapons`, `Armor`, `Props`, `Cloths` | the owner's own work (`.ztl`, `.spp`, `.blend`, `.fbx`, small Unreal projects per creature) |
| 13.74 | `ToS\Assets - 3th\Marcus Whinney` (extracted) | purchased creature models |
| 3.91 | the live `ToS` trees: `Tools\heightmap-generator` 1.80, `Plugins\AINiagara` 0.77, `Server\ToS-Server-5` 0.72, `Client` sources/config/content 0.45, `Design`, `Cutscenes` | |
| 0.83 | `GenericServer` (content and sources), `SteamSDk`, `Substance\previews` | `G:\Substance\previews` is the only folder written recently (2026-10-29, 0.06 GiB); `G:\Substance\Cache` is empty |
| 0.43 + 0.13 + ... | `Saved` subfolders that are state, not output (`Config`, `Collections`, `SourceControl`, `MapExports`, `AutoScreenshot.png`) | never targets |

Two observations for the owner, not for MyClean:

- `G:\ToS\Client`, the live Unreal client, is **not under version control** on G: (no `.git`
  in it or above it; the `ToS_Network` plugin's `libsodium` has a `.git` file pointing at a
  missing parent repository, so the plugin was copied out of a repository). Its `Content` is
  only 0.13 GiB, while the 2025 backup of the client has 58.26 GiB of content.
- `git status` in `G:\ToS\Server` lists 912 changed or untracked entries against the last commit
  (2025-11-28). Line-ending changes could explain part of it; this analysis did not look further.
