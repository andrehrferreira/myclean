# 05 Programs (99.7 GiB in Program Files, ~15 GiB of Store apps)

There are 86 visible entries in the Uninstall keys (HKLM, its 32-bit view and HKCU). Sizes
below are from the map. "Date" is the program folder's last-write time. Prefetch, which would
tell when each program last ran, is not readable without admin.

## C:\Program Files: 64.1 GiB

| GiB | Folder | Date | What | Candidate |
|---:|---|---|---|---|
| 15.7 | Microsoft Visual Studio\2022 | 2021-12 | VS Community 2022 17.14 | Modify: drop workloads not used (UWP, Xamarin/Android, C++ Clang if unused) |
| 12.0 | Adobe | 2026-10 | Photoshop 2026 5.4, Substance 3D Painter 5.0, Acrobat DC 1.2 | used (installed 2026-10) |
| 5.9 | NVIDIA GPU Computing Toolkit\CUDA | 2025-09 | CUDA toolkit(s) | uninstall old CUDA versions |
| 4.3 | NVIDIA Corporation | 2026-06 | Nsight Systems 2025.3.2 1.25, **Nsight Compute 2023.1.0** 1.00, **Nsight Systems 2023.1.2** 0.81, … | uninstall the 2023 Nsight versions: 1.8 |
| 4.0 | Common Files | 2026-09 | Adobe 3.4, Reallusion 0.3 | — |
| 3.4 | IK Multimedia\AmpliTube 5 | 2026-05 | AmpliTube 5 (plus its 3.2 GiB installer zip in Documents, 09) | the owner's |
| 3.3 | dotnet | 2026-02 | .NET SDKs 6.0.114 and 9.0.203, shared runtimes | **uninstall SDK 6.0.114** (.NET 6 is out of support): ~0.8 |
| 3.3 | Docker | 2026-10 | Docker Desktop | the owner's |
| 1.8 | Autodesk | 2026-08 | AdODIS 1.3, Genuine Service 0.3 | goes with the Autodesk apps |
| 1.3 | NVIDIA\CUDNN | 2025-10 | cuDNN | with CUDA |
| 1.0 | Google\Chrome | 2022-10 | Chrome | — |
| 0.8 | WSL | 2025-10 | WSL | — |
| 0.7–0.3 each | Bridge, TML, obs-studio, Ledger Live, OpenSSL-Win64 (2022), OpenVPN Connect, ASUS, Elgato, Git, Firefox, OpenJDK 11.0.16 (2022), Microsoft jdk-11.0.16 | | | two JDK 11 copies from 2022 |

## C:\Program Files (x86): 35.6 GiB

| GiB | Folder | Date | What | Candidate |
|---:|---|---|---|---|
| 5.5 | Microsoft SDKs | 2025-06 | **UWPNuGetPackages 3.5**, Windows Kits 1.2, NuGetPackages 0.8 | goes with the UWP workload |
| 5.2 | Windows Kits\10 | 2025-06 | **six Windows SDKs**: 10.0.16299, 19041, 22621.2428, 22621.5040, 26100.3916, 26100.7705 (estimated 1.9–2.6 each) | keep the newest one or two; uninstall 16299, 19041 and 22621.2428: **~6.6** by the Uninstall estimates |
| 4.8 | Microsoft Visual Studio | 2026-03 | 2022 3.4 (Installer, shared), **2019 1.1** (VS Community 2019 16.11 installed) | **uninstall VS 2019** if nothing needs v142 tooling |
| 4.4 | Steam | 2026-11 | client 1.7, `userdata` 2.75 (screenshots, cloud saves) | — |
| 3.2 | Android | 2025-06 | AndroidNDK 2.4, android-sdk 0.5, OpenJDK 0.3 (VS Xamarin/MAUI) | goes with the VS mobile workload |
| 2.1 | ASUS | 2026-09 | ArmouryDevice 1.2, **AsusGamingCenterInstallPackage 0.67** (an installer bundle) | — |
| 1.9 | Microsoft\Edge | | Edge | — |
| 1.7 | Common Files | | Autodesk Shared 0.9, Adobe 0.6 | — |
| 1.1 | dotnet (x86) | | x86 runtimes | — |
| 0.8 | Epic Games\Epic Online Services | 2024-10 | the games' service | — |
| 0.7 | Razer | | Synapse 3, Razer Services | — |
| 0.7 | Kaspersky Lab | | antivirus | — |
| 0.6 | NVIDIA Corporation | | Nsight Visual Studio Edition 2025.3 | — |
| 0.5 | Google\GoogleUpdater | 2024-04 | `crx_cache` 0.49 | Program Files is deny-listed, so MyClean leaves it |

The deny-list protects every `Program Files*` folder, so MyClean never deletes inside them.
Reclaim there is only by uninstalling, or by compressing.

## Compression (2)

`scan-all` estimates that LZX compression of `C:\Program Files (x86)` frees **18.2 GiB** (95 %
range 16.8–19.7). The analyzer's own exclusions leave out recently changed files and media.
CompressFolder applies it; undo is decompression.

## Uninstall candidates (3), with the Uninstall keys' estimates

| Est. GiB | Program | Why |
|---:|---|---|
| ~6.6 | Windows SDK 10.0.16299, 10.0.19041, 10.0.22621.2428 | superseded by 22621.5040 and 26100.x |
| ~5.9 + 1.3 | CUDA toolkit versions / cuDNN not used by current builds | check `nvcc --version` in projects first |
| ~3.5 + 3.2 + part of 15.7 | VS workloads UWP and mobile (UWPNuGetPackages, AndroidNDK) | VS Installer > Modify |
| ~1.1 + more | Visual Studio Community 2019 | VS 2022 is installed |
| ~1.8 | Nsight Compute 2023.1.0, Nsight Systems 2023.1.2 | newer 2025 versions are installed |
| ~0.8 + 0.4 | .NET SDK 6.0.114, Desktop Runtime 3.1.32, .NET Runtime 5.0.17 | out of support |
| ~0.6 | two OpenJDK/Microsoft JDK 11.0.16 copies (2022) | if nothing uses Java 11 |
| ~4.2 | Unity ArtEngine 2022.7.2 (`%APPDATA%\.artomatix`, discontinued by Unity) | not changed since 2023-06 |
| 6.6 | AmpliTube 5 + its installer zip | only if no longer used |

Total, if all of them go: **~30 GiB**.

## Store apps (WindowsApps, not visible): ~15 GiB

OpenAI Codex 2.3, Photos 0.9, Armoury Crate 0.65, AMD Radeon Software 0.65 (the AMD iGPU), WinDbg
0.64, Whiteboard 0.58, Snipping Tool 0.57, MovieMaker PRO and FREE 0.48 + 0.42, HP Printer
Control 0.43. Uninstall them in Settings > Apps, or move a Store app to another drive there (3).
