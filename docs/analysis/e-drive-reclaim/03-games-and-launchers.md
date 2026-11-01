# 03 Games and launchers

E: holds one Steam library, `E:\SteamLibrary` (172.8 GiB). No Epic, GOG, Xbox, EA or Riot
library is on E:, and `E:\WindowsApps` holds no package of the owner (`Get-AppxPackage` lists
none installed on E:). The Steam client itself is on C: (`%ProgramFiles(x86)%\Steam`, whose
`libraryfolder.vdf` points at this library).

| GiB | What | appmanifest | Status |
|---:|---|---|---|
| 140.4 | **Path of Exile 2** (`Bundles2` 122.3, `FMOD` 14.5, `Art` 3.4) | app 2694490, `SizeOnDisk` 150.7 GB (= the folder), installed or updated 2026-10-14, **`LastPlayed` 0** (never started from this Steam), `StateFlags` 6: an update of 89 MB is waiting | 3 (uninstall) or 2 (move to another library) |
| 31.2 | **No Man's Sky** | app 275850, last played 2026-09-20, updated 2026-10-06, an update of 7.5 GB is waiting | 3 / 2 |
| 0.9 | Blender (Steam build) | app 365670, last played 2026-01-28, updated 2026-10-25 | – |
| 0.17 | `steamapps\shadercache\275850` (No Man's Sky's shader pre-cache, 2026-10-03) | | 1 after a code change (07, 09) |
| 0.00 | `steamapps\common\Wardogs\Wardogs\.sentry-native` (5 files) | no manifest: the game was uninstalled and left its crash-reporter folder | 3 (delete the folder) |
| 0.00 | `steamapps\downloading`, `steamapps\temp` | empty | – |

Steam's sizes and the folder sizes agree, so there is no hidden leftover inside the games. Path
of Exile 2 keeps its game data in a few bundle files that every patch rewrites; it has no
separate cache folder on E: (`logs` is 2 files).

## Why nothing here is automatic

Game installs are the owner's (directive). What could be automatic are the launchers' caches:

- Steam's per-library **shader pre-cache** (`<library>\steamapps\shadercache\<appid>`) holds
  shaders Steam downloads or the game compiles again (to be confirmed against Valve's
  documentation, 07); it belongs to the `gpu-shader-caches`
  category, which the cleaning spec makes auto. Only 0.17 GiB here.
- Partly downloaded updates (`steamapps\downloading`) and staging (`steamapps\temp`) are empty
  now. Two updates are pending (7.5 GB for No Man's Sky), so a rule there could only throw away
  progress Steam would download again; not proposed.

The existing `games/steam-httpcache` and `games/steam-depotcache` rules name only
`%ProgramFiles(x86)%\Steam`, and clean targets cannot name "every drive": `?:\` works in
attribution and relocation locations (`check_locations(..., any_drive)` in
`src/core/packs/parse.cpp`), but the clean engine drops a target without a real drive (`target_entries` in `src/core/clean/engine.cpp`: "a target
must be an absolute path"). So a library on E: can only be cleaned by a target that spells
`E:\`, which a shipped pack must not do. The proposal in 07 is written for `?:\SteamLibrary`
and needs the engine change in 09 (preferably: expand Steam libraries from
`libraryfolders.vdf`, which `src/core/games/steam.cpp` already parses for the governor).

## Attribution

The map explains the whole library as `games/steam-library` ("Uninstall games you no longer
play, or move them in Steam > Settings > Storage"). That is right; no change.
