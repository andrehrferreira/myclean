# 16 Appendix: every folder of at least 256 MiB on C:

From the read-only `myclean map C:\ --min-gb 0.25 --depth 9` of 2026-11-01 01:00 (after the 00:49 cleaning run). The map keeps folders of at least 256 MiB (268 MB), slightly above the 200 MB asked: a second whole-drive walk at a lower threshold was not run (machine-load rule). A folder whose single subfolder holds 95 % or more of it, under the same area, is shown only as that subfolder. "Area" is the knowledge-pack area that explains it as of this map (before this task's pack changes; the new areas are in 13); "reclaim" its kind. "Folder date" is the folder's own last-write time (changes when entries are added or removed in it), not the newest file inside.


## C:\ (root)

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 445.84 | `C:\Users` | (no area) |  | 2026-09-30 |
| 3.24 | `C:\Autodesk\WI` | creative-apps/autodesk-installers | manual-review | 2026-06-20 |
| 0.32 | `C:\Autodesk\WI\14342801743708855898` | creative-apps/autodesk-installers | manual-review | 2023-10-13 |
| 3.08 | `C:\vcpkg` | vcpkg/root | manual-review | 2026-10-31 |
| 2.04 | `C:\vcpkg\downloads` | vcpkg/build-files | clean-rule | 2026-11-01 |
| 1.83 | `C:\vcpkg\downloads\tools` | vcpkg/build-files | clean-rule | 2026-10-31 |
| 0.85 | `C:\vcpkg\downloads\tools\perl\5.38.0.1` | vcpkg/build-files | clean-rule | 2024-08-07 |
| 0.68 | `C:\vcpkg\downloads\tools\perl\5.38.0.1\c` | vcpkg/build-files | clean-rule | 2024-08-07 |
| 0.60 | `C:\vcpkg\buildtrees` | vcpkg/build-files | clean-rule | 2026-11-01 |

## C:\Windows

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 49.83 | `C:\Windows` | windows/system | none | 2026-10-15 |
| 11.30 | `C:\Windows\System32` | windows/system | none | 2026-10-19 |
| 6.10 | `C:\Windows\System32\DriverStore\FileRepository` | windows/driver-store | clean-rule | 2026-10-01 |
| 2.53 | `C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_da865124972e1f80` | windows/driver-store | clean-rule | 2026-09-30 |
| 0.92 | `C:\Windows\System32\DriverStore\FileRepository\u0405277.inf_amd64_00f6bd87014da3b2\B404941` | windows/driver-store | clean-rule | 2026-09-30 |
| 0.50 | `C:\Windows\System32\DriverStore\FileRepository\awcc_awccappandwin32_dchu_component.inf_amd64_1334f793d7f7ad2d` | windows/driver-store | clean-rule | 2026-09-30 |
| 0.47 | `C:\Windows\System32\DriverStore\FileRepository\amdocl.inf_amd64_ddfc6cd36ec0ac4c` | windows/driver-store | clean-rule | 2026-09-30 |
| 0.64 | `C:\Windows\System32\Microsoft-Edge-WebView` | windows/system | none | ? |
| 10.95 | `C:\Windows\Installer` | windows/installer-cache | manual-review | 2026-10-30 |
| 2.49 | `C:\Windows\Installer\Razer Central` | windows/installer-cache | manual-review | 2022-10-12 |
| 0.71 | `C:\Windows\Installer\$PatchCache$\Managed` | windows/installer-cache | manual-review | 2026-09-20 |
| 0.52 | `C:\Windows\Installer\$PatchCache$\Managed\68AB67CA640133017706CB5110E47A00\21.1.20135` | windows/installer-cache | manual-review | 2026-08-14 |
| 0.48 | `C:\Windows\Installer\Razer\Installer` | windows/installer-cache | manual-review | 2021-12-14 |
| 9.12 | `C:\Windows\WinSxS` | windows/component-store | clean-rule | 2026-10-13 |
| 0.57 | `C:\Windows\WinSxS\amd64_microsoft-edge-webview_31bf3856ad364e35_10.0.26100.6584_none_2eacdaf93ac57bd8` | windows/component-store | clean-rule | 2025-09-15 |
| 0.52 | `C:\Windows\WinSxS\amd64_microsoft-edge-webview_31bf3856ad364e35_10.0.26100.1_none_8fe1c356f34a19ca` | windows/component-store | clean-rule | 2024-04-01 |
| 0.47 | `C:\Windows\WinSxS\Backup` | windows/component-store | clean-rule | 2026-09-30 |
| 7.30 | `C:\Windows\SoftwareDistribution\Download` | windows/update-downloads | manual-review | 2026-10-31 |
| 6.38 | `C:\Windows\SoftwareDistribution\Download\9cd825481fc54804` | windows/update-downloads | manual-review | 2026-10-13 |
| 5.69 | `C:\Windows\SoftwareDistribution\Download\9cd825481fc54804\Package_for_RollupFix~~amd64~~26100.9711.1.30` | windows/update-downloads | manual-review | 2026-10-13 |
| 1.03 | `C:\Windows\SoftwareDistribution\Download\9cd825481fc54804\Package_for_RollupFix~~amd64~~26100.9711.1.30\amd64_microsoft-edge-webview_31bf3856ad364e35_10.0.26100.9711_none_2e9083253adbaceb` | windows/update-downloads | manual-review | 2026-10-13 |
| 0.26 | `C:\Windows\SoftwareDistribution\Download\9cd825481fc54804\Package_for_RollupFix~~amd64~~26100.9711.1.30\amd64_microsoft-edge-webview_31bf3856ad364e35_10.0.26100.9711_none_2e9083253adbaceb\n` | windows/update-downloads | manual-review | 2026-10-13 |
| 0.36 | `C:\Windows\SoftwareDistribution\Download\9cd825481fc54804\Metadata` | windows/update-downloads | manual-review | 2026-10-13 |
| 6.12 | `C:\Windows\assembly` | windows/assembly | none | 2026-10-01 |
| 3.23 | `C:\Windows\assembly\NativeImages_v4.0.30319_64` | windows/assembly | none | 2026-10-01 |
| 0.26 | `C:\Windows\assembly\NativeImages_v4.0.30319_64\Microsoft.C1d3c2215#` | windows/assembly | none | 2026-10-01 |
| 2.58 | `C:\Windows\assembly\NativeImages_v4.0.30319_32` | windows/assembly | none | 2026-10-01 |
| 1.38 | `C:\Windows\SysWOW64` | windows/system | none | 2026-10-19 |
| 0.93 | `C:\Windows\SystemApps` | windows/system | none | 2026-09-30 |
| 0.32 | `C:\Windows\SystemApps\MicrosoftWindows.Client.CBS_cw5n1h2txyewy` | windows/system | none | 2026-09-30 |
| 0.73 | `C:\Windows\Panther\monitor` | windows-setup-logs/panther | clean-rule | 2026-11-01 |
| 0.58 | `C:\Windows\Microsoft.NET` | windows/system | none | 2026-10-29 |
| 0.28 | `C:\Windows\Microsoft.NET\Framework` | windows/system | none | 2026-09-30 |
| 0.37 | `C:\Windows\Fonts` | windows/system | none | 2026-09-30 |

## C:\Program Files

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 64.10 | `C:\Program Files` | windows/program-files | uninstall | 2026-11-01 |
| 15.74 | `C:\Program Files\Microsoft Visual Studio\2022\Community` | visual-studio/installations | uninstall | 2026-02-25 |
| 10.83 | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools` | visual-studio/installations | uninstall | 2025-06-30 |
| 5.45 | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm` | visual-studio/installations | uninstall | 2025-06-30 |
| 1.64 | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin` | visual-studio/installations | uninstall | 2025-06-30 |
| 5.38 | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC` | visual-studio/installations | uninstall | 2025-06-30 |
| 2.04 | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207` | visual-studio/installations | uninstall | 2026-02-25 |
| 1.70 | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.36.32532` | visual-studio/installations | uninstall | 2023-12-01 |
| 1.64 | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130` | visual-studio/installations | uninstall | 2024-10-23 |
| 3.73 | `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE` | visual-studio/installations | uninstall | 2026-02-25 |
| 1.60 | `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft` | visual-studio/installations | uninstall | 2026-02-25 |
| 1.50 | `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\Extensions` | visual-studio/installations | uninstall | 2026-02-25 |
| 1.26 | `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\Extensions\Microsoft` | visual-studio/installations | uninstall | 2026-02-25 |
| 0.29 | `C:\Program Files\Microsoft Visual Studio\2022\Community\CoreCon\Binaries` | visual-studio/installations | uninstall | 2026-02-25 |
| 0.27 | `C:\Program Files\Microsoft Visual Studio\2022\Community\CoreCon\Binaries\Phone Tools\Debugger` | visual-studio/installations | uninstall | 2026-02-25 |
| 11.98 | `C:\Program Files\Adobe` | windows/program-files | uninstall | 2026-10-13 |
| 5.44 | `C:\Program Files\Adobe\Adobe Photoshop 2026` | windows/program-files | uninstall | 2026-10-08 |
| 3.52 | `C:\Program Files\Adobe\Adobe Photoshop 2026\Required` | windows/program-files | uninstall | 2026-10-04 |
| 1.00 | `C:\Program Files\Adobe\Adobe Photoshop 2026\Required\DynamicLinkMediaServer` | windows/program-files | uninstall | 2026-10-04 |
| 0.42 | `C:\Program Files\Adobe\Adobe Photoshop 2026\Required\DynamicLinkMediaServer\OpenColorIO-Configs\ACES 1.2\luts` | windows/program-files | uninstall | 2026-10-04 |
| 0.99 | `C:\Program Files\Adobe\Adobe Photoshop 2026\Required\sensei_models` | windows/program-files | uninstall | 2026-10-04 |
| 0.45 | `C:\Program Files\Adobe\Adobe Photoshop 2026\Required\UXP` | windows/program-files | uninstall | 2026-10-08 |
| 0.37 | `C:\Program Files\Adobe\Adobe Photoshop 2026\Required\CEP` | windows/program-files | uninstall | 2026-10-04 |
| 0.27 | `C:\Program Files\Adobe\Adobe Photoshop 2026\Required\CEP\CEPHtmlEngine` | windows/program-files | uninstall | 2026-10-04 |
| 4.95 | `C:\Program Files\Adobe\Adobe Substance 3D Painter` | windows/program-files | uninstall | 2026-10-21 |
| 3.52 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources` | windows/program-files | uninstall | 2026-10-21 |
| 2.12 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\starter_assets` | windows/program-files | uninstall | 2026-10-21 |
| 0.78 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\starter_assets\smart-materials` | windows/program-files | uninstall | 2026-10-21 |
| 0.31 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\starter_assets\smart-materials\Metal` | windows/program-files | uninstall | 2026-10-21 |
| 0.31 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\starter_assets\templates` | windows/program-files | uninstall | 2026-10-21 |
| 0.29 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\starter_assets\presets` | windows/program-files | uninstall | 2026-10-21 |
| 0.69 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\ocio` | windows/program-files | uninstall | 2026-10-21 |
| 0.42 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\ocio\aces_1.2\luts` | windows/program-files | uninstall | 2026-10-21 |
| 0.26 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\ocio\aces_1.0.3\luts` | windows/program-files | uninstall | 2026-10-21 |
| 0.46 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\resources\samples` | windows/program-files | uninstall | 2026-10-21 |
| 0.57 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\PlugIns` | windows/program-files | uninstall | 2026-10-21 |
| 0.29 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\PlugIns\iray` | windows/program-files | uninstall | 2026-10-21 |
| 0.28 | `C:\Program Files\Adobe\Adobe Substance 3D Painter\PlugIns\vectorialrendererplugin\aiserver` | windows/program-files | uninstall | 2026-10-21 |
| 1.20 | `C:\Program Files\Adobe\Acrobat DC\Acrobat` | windows/program-files | uninstall | 2026-10-19 |
| 0.42 | `C:\Program Files\Adobe\Acrobat DC\Acrobat\WebResources` | windows/program-files | uninstall | 2026-10-19 |
| 0.31 | `C:\Program Files\Adobe\Acrobat DC\Acrobat\WebResources\SA` | windows/program-files | uninstall | 2026-03-26 |
| 5.90 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA` | dev-tools/sdks | uninstall | 2025-11-05 |
| 3.06 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6` | dev-tools/sdks | uninstall | 2025-09-28 |
| 1.79 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin` | dev-tools/sdks | uninstall | 2025-09-28 |
| 1.00 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\lib\x64` | dev-tools/sdks | uninstall | 2025-09-28 |
| 2.83 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.1` | dev-tools/sdks | uninstall | 2025-11-05 |
| 1.88 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.1\bin` | dev-tools/sdks | uninstall | 2025-11-05 |
| 0.68 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.1\lib\x64` | dev-tools/sdks | uninstall | 2025-11-05 |
| 4.29 | `C:\Program Files\NVIDIA Corporation` | windows/program-files | uninstall | 2026-06-06 |
| 1.25 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2025.3.2` | windows/program-files | uninstall | 2025-09-28 |
| 0.51 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2025.3.2\target-linux-x64` | windows/program-files | uninstall | 2025-09-28 |
| 0.46 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2025.3.2\host-windows-x64` | windows/program-files | uninstall | 2025-09-28 |
| 0.26 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2025.3.2\target-windows-x64` | windows/program-files | uninstall | 2025-09-28 |
| 1.00 | `C:\Program Files\NVIDIA Corporation\Nsight Compute 2023.1.0` | windows/program-files | uninstall | 2025-11-05 |
| 0.62 | `C:\Program Files\NVIDIA Corporation\Nsight Compute 2023.1.0\host` | windows/program-files | uninstall | 2025-11-05 |
| 0.48 | `C:\Program Files\NVIDIA Corporation\Nsight Compute 2023.1.0\host\windows-desktop-win7-x64` | windows/program-files | uninstall | 2025-11-05 |
| 0.31 | `C:\Program Files\NVIDIA Corporation\Nsight Compute 2023.1.0\target` | windows/program-files | uninstall | 2025-11-05 |
| 0.81 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2023.1.2` | windows/program-files | uninstall | 2025-11-05 |
| 0.37 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2023.1.2\host-windows-x64` | windows/program-files | uninstall | 2025-11-05 |
| 0.28 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2023.1.2\target-linux-x64` | windows/program-files | uninstall | 2025-11-05 |
| 0.59 | `C:\Program Files\NVIDIA Corporation\NVIDIA app` | windows/program-files | uninstall | 2026-09-30 |
| 0.37 | `C:\Program Files\NVIDIA Corporation\NVIDIA app\CEF` | windows/program-files | uninstall | 2026-09-30 |
| 0.48 | `C:\Program Files\NVIDIA Corporation\NVIDIA Broadcast` | windows/program-files | uninstall | 2026-06-06 |
| 3.96 | `C:\Program Files\Common Files` | windows/program-files | uninstall | 2026-09-30 |
| 3.36 | `C:\Program Files\Common Files\Adobe` | windows/program-files | uninstall | 2026-10-27 |
| 0.76 | `C:\Program Files\Common Files\Adobe\Adobe Photoshop 2026\AddOnModules\sensei_model_cache\inpainting_ai\super_caf` | windows/program-files | uninstall | 2026-01-30 |
| 0.72 | `C:\Program Files\Common Files\Adobe\Acrobat\Setup\{AC76BA86-1046-1033-7760-BC15014EA700}` | windows/program-files | uninstall | 2021-12-14 |
| 0.58 | `C:\Program Files\Common Files\Adobe\Microsoft\EdgeWebView` | windows/program-files | uninstall | 2025-12-08 |
| 0.32 | `C:\Program Files\Common Files\Adobe\Adobe Desktop Common` | windows/program-files | uninstall | 2026-10-13 |
| 0.31 | `C:\Program Files\Common Files\Adobe\CEP\extensions` | windows/program-files | uninstall | 2026-10-27 |
| 0.30 | `C:\Program Files\Common Files\Adobe\Plug-Ins\CC\File Formats` | windows/program-files | uninstall | 2026-10-04 |
| 0.32 | `C:\Program Files\Common Files\Reallusion` | windows/program-files | uninstall | 2023-01-05 |
| 3.43 | `C:\Program Files\IK Multimedia\AmpliTube 5` | windows/program-files | uninstall | 2026-05-16 |
| 3.31 | `C:\Program Files\dotnet` | dev-tools/sdks | uninstall | 2026-02-25 |
| 1.39 | `C:\Program Files\dotnet\shared` | dev-tools/sdks | uninstall | 2024-03-21 |
| 0.61 | `C:\Program Files\dotnet\shared\Microsoft.WindowsDesktop.App` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.61 | `C:\Program Files\dotnet\shared\Microsoft.NETCore.App` | dev-tools/sdks | uninstall | 2026-02-25 |
| 1.06 | `C:\Program Files\dotnet\sdk` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.38 | `C:\Program Files\dotnet\sdk\9.0.311` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.38 | `C:\Program Files\dotnet\sdk\9.0.203` | dev-tools/sdks | uninstall | 2025-04-10 |
| 0.30 | `C:\Program Files\dotnet\sdk\6.0.114` | dev-tools/sdks | uninstall | 2023-02-17 |
| 0.81 | `C:\Program Files\dotnet\packs` | dev-tools/sdks | uninstall | 2021-12-14 |
| 3.30 | `C:\Program Files\Docker\Docker` | windows/program-files | uninstall | 2026-10-24 |
| 2.84 | `C:\Program Files\Docker\Docker\resources` | windows/program-files | uninstall | 2026-10-24 |
| 0.87 | `C:\Program Files\Docker\Docker\resources\wsl` | windows/program-files | uninstall | 2026-10-24 |
| 0.64 | `C:\Program Files\Docker\Docker\resources\cli-plugins` | windows/program-files | uninstall | 2026-10-24 |
| 0.41 | `C:\Program Files\Docker\Docker\frontend` | windows/program-files | uninstall | 2026-10-24 |
| 1.79 | `C:\Program Files\Autodesk` | windows/program-files | uninstall | 2026-08-18 |
| 1.27 | `C:\Program Files\Autodesk\AdODIS\V1` | windows/program-files | uninstall | 2026-03-12 |
| 1.16 | `C:\Program Files\Autodesk\AdODIS\V1\Setup` | windows/program-files | uninstall | 2026-03-02 |
| 0.32 | `C:\Program Files\Autodesk\AdODIS\V1\Setup\ui-launcher` | windows/program-files | uninstall | 2026-03-02 |
| 0.32 | `C:\Program Files\Autodesk\Genuine Service` | windows/program-files | uninstall | 2025-12-25 |
| 1.25 | `C:\Program Files\NVIDIA\CUDNN\v9.14\bin` | windows/program-files | uninstall | 2025-10-22 |
| 0.87 | `C:\Program Files\NVIDIA\CUDNN\v9.14\bin\12.9` | windows/program-files | uninstall | 2025-10-22 |
| 0.39 | `C:\Program Files\NVIDIA\CUDNN\v9.14\bin\13.0` | windows/program-files | uninstall | 2025-10-22 |
| 1.04 | `C:\Program Files\Google\Chrome\Application` | windows/program-files | uninstall | 2026-10-29 |
| 0.46 | `C:\Program Files\Google\Chrome\Application\156.0.8078.76` | windows/program-files | uninstall | 2026-10-29 |
| 0.46 | `C:\Program Files\Google\Chrome\Application\154.0.8037.92` | windows/program-files | uninstall | 2026-09-29 |
| 0.82 | `C:\Program Files\WSL` | windows/program-files | uninstall | 2025-10-02 |
| 0.70 | `C:\Program Files\Bridge` | windows/program-files | uninstall | 2023-08-14 |
| 0.47 | `C:\Program Files\Bridge\resources` | windows/program-files | uninstall | 2023-08-14 |
| 0.53 | `C:\Program Files\TML` | windows/program-files | uninstall | 2026-04-14 |
| 0.42 | `C:\Program Files\TML\plugins` | windows/program-files | uninstall | 2026-04-14 |
| 0.50 | `C:\Program Files\obs-studio` | windows/program-files | uninstall | 2022-01-22 |
| 0.31 | `C:\Program Files\obs-studio\obs-plugins\64bit` | windows/program-files | uninstall | 2026-05-21 |
| 0.49 | `C:\Program Files\Ledger Live\Ledger Wallet` | windows/program-files | uninstall | 2026-10-24 |
| 0.42 | `C:\Program Files\OpenSSL-Win64` | windows/program-files | uninstall | 2022-08-13 |
| 0.36 | `C:\Program Files\OpenVPN Connect` | windows/program-files | uninstall | 2026-09-29 |
| 0.36 | `C:\Program Files\ASUS` | windows/program-files | uninstall | 2026-09-30 |
| 0.35 | `C:\Program Files\Elgato` | windows/program-files | uninstall | 2026-08-18 |
| 0.30 | `C:\Program Files\Elgato\4KCaptureUtility` | windows/program-files | uninstall | 2022-11-09 |
| 0.34 | `C:\Program Files\Git` | windows/program-files | uninstall | 2026-07-16 |
| 0.31 | `C:\Program Files\Mozilla Firefox` | windows/program-files | uninstall | 2026-01-06 |
| 0.29 | `C:\Program Files\OpenJDK\openjdk-11.0.16_8` | windows/program-files | uninstall | 2022-07-16 |
| 0.29 | `C:\Program Files\Microsoft\jdk-11.0.16.101-hotspot` | windows/program-files | uninstall | 2023-06-22 |

## C:\Program Files (x86)

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 35.60 | `C:\Program Files (x86)` | windows/program-files | uninstall | 2026-10-30 |
| 5.47 | `C:\Program Files (x86)\Microsoft SDKs` | windows/program-files | uninstall | 2025-06-30 |
| 3.48 | `C:\Program Files (x86)\Microsoft SDKs\UWPNuGetPackages` | windows/program-files | uninstall | 2023-06-22 |
| 0.34 | `C:\Program Files (x86)\Microsoft SDKs\UWPNuGetPackages\runtime.win10-x64.microsoft.net.native.sharedlibrary\2.2.8-rel-31116-00\tools\SharedLibrary` | windows/program-files | uninstall | 2023-06-22 |
| 0.32 | `C:\Program Files (x86)\Microsoft SDKs\UWPNuGetPackages\runtime.win10-arm.microsoft.net.native.sharedlibrary\2.2.8-rel-31116-00\tools\SharedLibrary` | windows/program-files | uninstall | 2023-06-22 |
| 0.32 | `C:\Program Files (x86)\Microsoft SDKs\UWPNuGetPackages\runtime.win10-x86.microsoft.net.native.sharedlibrary\2.2.8-rel-31116-00\tools\SharedLibrary` | windows/program-files | uninstall | 2023-06-22 |
| 0.30 | `C:\Program Files (x86)\Microsoft SDKs\UWPNuGetPackages\microsoft.net.native.sharedlibrary-x64\1.7.3\tools\SharedLibrary` | windows/program-files | uninstall | 2023-06-22 |
| 0.29 | `C:\Program Files (x86)\Microsoft SDKs\UWPNuGetPackages\runtime.win10-arm64.microsoft.net.native.sharedlibrary\2.2.8-rel-31116-00\tools\SharedLibrary` | windows/program-files | uninstall | 2023-06-22 |
| 0.29 | `C:\Program Files (x86)\Microsoft SDKs\UWPNuGetPackages\microsoft.net.native.sharedlibrary-arm\1.7.3\tools\SharedLibrary` | windows/program-files | uninstall | 2023-06-22 |
| 0.28 | `C:\Program Files (x86)\Microsoft SDKs\UWPNuGetPackages\microsoft.net.native.sharedlibrary-x86\1.7.3\tools\SharedLibrary` | windows/program-files | uninstall | 2023-06-22 |
| 1.16 | `C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs` | windows/program-files | uninstall | 2025-06-30 |
| 0.84 | `C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.NET.Native.Framework.1.3\1.3` | windows/program-files | uninstall | 2021-12-14 |
| 0.28 | `C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.NET.Native.Framework.1.3\1.3\x64` | windows/program-files | uninstall | 2021-12-14 |
| 0.28 | `C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.NET.Native.Framework.1.3\1.3\arm` | windows/program-files | uninstall | 2021-12-14 |
| 0.27 | `C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.NET.Native.Framework.1.3\1.3\x86` | windows/program-files | uninstall | 2021-12-14 |
| 0.77 | `C:\Program Files (x86)\Microsoft SDKs\NuGetPackages` | windows/program-files | uninstall | 2025-06-30 |
| 5.19 | `C:\Program Files (x86)\Windows Kits\10` | dev-tools/sdks | uninstall | 2026-02-25 |
| 2.41 | `C:\Program Files (x86)\Windows Kits\10\Lib` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.93 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.53 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\um` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.40 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\ucrt` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.75 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.42 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.33 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.72 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.46 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0\um` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.27 | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0\ucrt` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.96 | `C:\Program Files (x86)\Windows Kits\10\Include` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.35 | `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.32 | `C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.30 | `C:\Program Files (x86)\Windows Kits\10\Include\10.0.19041.0` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.69 | `C:\Program Files (x86)\Windows Kits\10\bin` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.26 | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0` | dev-tools/sdks | uninstall | 2025-06-30 |
| 0.44 | `C:\Program Files (x86)\Windows Kits\10\References` | dev-tools/sdks | uninstall | 2026-02-25 |
| 0.43 | `C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit` | dev-tools/sdks | uninstall | 2025-06-30 |
| 4.78 | `C:\Program Files (x86)\Microsoft Visual Studio` | visual-studio/installations | uninstall | 2026-03-04 |
| 3.37 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` | visual-studio/installations | uninstall | 2026-02-25 |
| 2.00 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools` | visual-studio/installations | uninstall | 2025-06-30 |
| 1.84 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207` | visual-studio/installations | uninstall | 2026-02-25 |
| 1.02 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE` | visual-studio/installations | uninstall | 2026-02-25 |
| 0.55 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions` | visual-studio/installations | uninstall | 2026-02-25 |
| 0.51 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft` | visual-studio/installations | uninstall | 2026-02-25 |
| 1.13 | `C:\Program Files (x86)\Microsoft Visual Studio\2019\Community` | visual-studio/installations | uninstall | 2025-04-11 |
| 1.01 | `C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7` | visual-studio/installations | uninstall | 2025-04-11 |
| 0.83 | `C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE` | visual-studio/installations | uninstall | 2025-04-11 |
| 0.52 | `C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft` | visual-studio/installations | uninstall | 2025-04-11 |
| 4.44 | `C:\Program Files (x86)\Steam` | games/steam-client | none | 2026-11-01 |
| 0.66 | `C:\Program Files (x86)\Steam\bin` | games/steam-client | none | 2026-10-03 |
| 0.61 | `C:\Program Files (x86)\Steam\bin\cef` | games/steam-client | none | 2025-12-25 |
| 0.33 | `C:\Program Files (x86)\Steam\bin\cef\cef.win64` | games/steam-client | none | 2026-10-03 |
| 0.27 | `C:\Program Files (x86)\Steam\bin\cef\cef.win7` | games/steam-client | none | 2025-11-02 |
| 0.43 | `C:\Program Files (x86)\Steam\package` | games/steam-client | none | 2026-10-31 |
| 3.24 | `C:\Program Files (x86)\Android` | windows/program-files | uninstall | 2025-06-30 |
| 2.43 | `C:\Program Files (x86)\Android\AndroidNDK\android-ndk-r23c` | windows/program-files | uninstall | 2025-06-30 |
| 2.06 | `C:\Program Files (x86)\Android\AndroidNDK\android-ndk-r23c\toolchains\llvm\prebuilt\windows-x86_64` | windows/program-files | uninstall | 2025-06-30 |
| 0.52 | `C:\Program Files (x86)\Android\android-sdk` | windows/program-files | uninstall | 2025-06-30 |
| 0.29 | `C:\Program Files (x86)\Android\openjdk\jdk-17.0.14` | windows/program-files | uninstall | 2025-06-30 |
| 2.05 | `C:\Program Files (x86)\ASUS` | windows/program-files | uninstall | 2026-09-17 |
| 1.24 | `C:\Program Files (x86)\ASUS\ArmouryDevice` | windows/program-files | uninstall | 2024-11-16 |
| 0.77 | `C:\Program Files (x86)\ASUS\ArmouryDevice\dll` | windows/program-files | uninstall | 2024-11-19 |
| 0.55 | `C:\Program Files (x86)\ASUS\ArmouryDevice\dll\AudioSDK` | windows/program-files | uninstall | 2024-11-16 |
| 0.33 | `C:\Program Files (x86)\ASUS\ArmouryDevice\dll\AudioSDK\InstallHelper` | windows/program-files | uninstall | 2024-11-16 |
| 0.31 | `C:\Program Files (x86)\ASUS\ArmouryDevice\dll\AudioSDK\InstallHelper\BongioviAPI` | windows/program-files | uninstall | 2024-11-16 |
| 0.26 | `C:\Program Files (x86)\ASUS\ArmouryDevice\View` | windows/program-files | uninstall | 2026-08-18 |
| 0.67 | `C:\Program Files (x86)\ASUS\AsusGamingCenterInstallPackage` | windows/program-files | uninstall | 2024-11-16 |
| 1.87 | `C:\Program Files (x86)\Microsoft` | windows/program-files | uninstall | 2026-09-30 |
| 1.76 | `C:\Program Files (x86)\Microsoft\Edge\Application` | windows/program-files | uninstall | 2026-10-28 |
| 0.58 | `C:\Program Files (x86)\Microsoft\Edge\Application\155.0.4283.45` | windows/program-files | uninstall | 2026-10-10 |
| 0.58 | `C:\Program Files (x86)\Microsoft\Edge\Application\155.0.4283.62` | windows/program-files | uninstall | 2026-10-18 |
| 0.58 | `C:\Program Files (x86)\Microsoft\Edge\Application\156.0.4314.50` | windows/program-files | uninstall | 2026-10-28 |
| 1.72 | `C:\Program Files (x86)\Common Files` | windows/program-files | uninstall | 2026-09-30 |
| 0.92 | `C:\Program Files (x86)\Common Files\Autodesk Shared` | windows/program-files | uninstall | 2023-10-13 |
| 0.73 | `C:\Program Files (x86)\Common Files\Autodesk Shared\AdskLicensing` | windows/program-files | uninstall | 2026-05-12 |
| 0.58 | `C:\Program Files (x86)\Common Files\Adobe` | windows/program-files | uninstall | 2026-10-13 |
| 1.14 | `C:\Program Files (x86)\dotnet` | windows/program-files | uninstall | 2026-02-25 |
| 0.85 | `C:\Program Files (x86)\dotnet\shared` | windows/program-files | uninstall | 2025-06-30 |
| 0.41 | `C:\Program Files (x86)\dotnet\shared\Microsoft.WindowsDesktop.App` | windows/program-files | uninstall | 2026-02-25 |
| 0.31 | `C:\Program Files (x86)\dotnet\shared\Microsoft.NETCore.App` | windows/program-files | uninstall | 2026-02-25 |
| 0.30 | `C:\Program Files (x86)\dotnet\packs` | windows/program-files | uninstall | 2025-06-30 |
| 0.79 | `C:\Program Files (x86)\Epic Games\Epic Online Services` | windows/program-files | uninstall | 2026-10-11 |
| 0.61 | `C:\Program Files (x86)\Epic Games\Epic Online Services\managedArtifacts\98bc04bc842e4906993fd6d6644ffb8d` | windows/program-files | uninstall | 2026-10-11 |
| 0.33 | `C:\Program Files (x86)\Epic Games\Epic Online Services\managedArtifacts\98bc04bc842e4906993fd6d6644ffb8d\Win64` | windows/program-files | uninstall | 2026-10-11 |
| 0.27 | `C:\Program Files (x86)\Epic Games\Epic Online Services\managedArtifacts\98bc04bc842e4906993fd6d6644ffb8d\Win32` | windows/program-files | uninstall | 2026-10-11 |
| 0.74 | `C:\Program Files (x86)\Razer` | windows/program-files | uninstall | 2026-09-30 |
| 0.37 | `C:\Program Files (x86)\Razer\Synapse3` | windows/program-files | uninstall | 2021-12-14 |
| 0.35 | `C:\Program Files (x86)\Razer\Synapse3\WPFUI` | windows/program-files | uninstall | 2021-12-14 |
| 0.29 | `C:\Program Files (x86)\Razer\Synapse3\WPFUI\Framework` | windows/program-files | uninstall | 2021-12-14 |
| 0.34 | `C:\Program Files (x86)\Razer\Razer Services` | windows/program-files | uninstall | 2021-12-14 |
| 0.72 | `C:\Program Files (x86)\Kaspersky Lab` | windows/program-files | uninstall | 2026-08-06 |
| 0.45 | `C:\Program Files (x86)\Kaspersky Lab\Kaspersky 21.26` | windows/program-files | uninstall | 2026-09-30 |
| 0.61 | `C:\Program Files (x86)\NVIDIA Corporation` | windows/program-files | uninstall | 2026-09-30 |
| 0.41 | `C:\Program Files (x86)\NVIDIA Corporation\Nsight Visual Studio Edition 2025.3` | windows/program-files | uninstall | 2025-09-28 |
| 0.57 | `C:\Program Files (x86)\Reference Assemblies\Microsoft\Framework` | windows/program-files | uninstall | 2026-09-30 |
| 0.52 | `C:\Program Files (x86)\Reference Assemblies\Microsoft\Framework\.NETFramework` | windows/program-files | uninstall | 2025-04-11 |
| 0.51 | `C:\Program Files (x86)\Google\GoogleUpdater\crx_cache` | windows/program-files | uninstall | 2026-10-29 |
| 0.35 | `C:\Program Files (x86)\InstallShield Installation Information` | windows/program-files | uninstall | 2026-08-18 |

## C:\ProgramData

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 35.87 | `C:\ProgramData` | (no area) |  | 2026-11-01 |
| 10.95 | `C:\ProgramData\Microsoft` | (no area) |  | 2026-09-30 |
| 10.27 | `C:\ProgramData\Microsoft\VisualStudio` | (no area) |  | 2025-01-25 |
| 10.26 | `C:\ProgramData\Microsoft\VisualStudio\Packages` | visual-studio/packages | manual-review | 2026-02-25 |
| 2.07 | `C:\ProgramData\Microsoft\VisualStudio\Packages\Microsoft.VisualStudio.VC.Llvm.Clang,version=17.14.36203.27,productarch=neutral` | visual-studio/packages | manual-review | 2025-06-30 |
| 0.90 | `C:\ProgramData\Microsoft\VisualStudio\Packages\Microsoft.Net.CoreUWP,version=1.1.56` | visual-studio/packages | manual-review | 2023-06-22 |
| 0.74 | `C:\ProgramData\Microsoft\VisualStudio\Packages\AndroidNDK_R23C,version=23.3,chip=x64` | visual-studio/packages | manual-review | 2025-06-30 |
| 0.65 | `C:\ProgramData\Microsoft\Windows Defender` | (no area) |  | 2026-09-30 |
| 0.47 | `C:\ProgramData\Microsoft\Windows Defender\Definition Updates` | (no area) |  | 2026-09-30 |
| 0.43 | `C:\ProgramData\Microsoft\Windows Defender\Definition Updates\Backup` | (no area) |  | 2026-09-30 |
| 5.16 | `C:\ProgramData\NVIDIA Corporation` | (no area) |  | 2026-06-06 |
| 4.65 | `C:\ProgramData\NVIDIA Corporation\NVIDIA App` | (no area) |  | 2026-09-30 |
| 4.54 | `C:\ProgramData\NVIDIA Corporation\NVIDIA App\UpdateFramework\ota-artifacts` | gpu/driver-installers | manual-review | 2026-09-30 |
| 3.68 | `C:\ProgramData\NVIDIA Corporation\NVIDIA App\UpdateFramework\ota-artifacts\grd` | gpu/driver-installers | manual-review | 2026-09-30 |
| 2.94 | `C:\ProgramData\NVIDIA Corporation\NVIDIA App\UpdateFramework\ota-artifacts\grd\post-processing\dc5ffd3eee1002c4977b00b0f6ffd21c` | gpu/driver-installers | manual-review | 2026-09-30 |
| 0.74 | `C:\ProgramData\NVIDIA Corporation\NVIDIA App\UpdateFramework\ota-artifacts\grd\dc5ffd3eee1002c4977b00b0f6ffd21c` | gpu/driver-installers | manual-review | 2026-09-30 |
| 0.86 | `C:\ProgramData\NVIDIA Corporation\NVIDIA App\UpdateFramework\ota-artifacts\nvapp` | gpu/driver-installers | manual-review | 2026-10-14 |
| 0.69 | `C:\ProgramData\NVIDIA Corporation\NVIDIA App\UpdateFramework\ota-artifacts\nvapp\post-processing\14ac30af580144f7ee3c1181c1854189` | gpu/driver-installers | manual-review | 2026-10-14 |
| 0.27 | `C:\ProgramData\NVIDIA Corporation\NVIDIA Broadcast` | (no area) |  | 2026-06-06 |
| 4.59 | `C:\ProgramData\NVIDIA\NGX\models` | (no area) |  | 2026-09-30 |
| 0.68 | `C:\ProgramData\NVIDIA\NGX\models\dlss\versions` | (no area) |  | 2026-09-04 |
| 0.45 | `C:\ProgramData\NVIDIA\NGX\models\dlssd\versions` | (no area) |  | 2026-09-04 |
| 2.76 | `C:\ProgramData\Package Cache` | windows/package-cache | manual-review | 2026-09-30 |
| 2.67 | `C:\ProgramData\Kaspersky Lab` | (no area) |  | 2026-08-06 |
| 1.69 | `C:\ProgramData\Kaspersky Lab\AVP21.26` | (no area) |  | 2026-09-30 |
| 0.85 | `C:\ProgramData\Kaspersky Lab\AVP21.26\Bases` | (no area) |  | 2026-10-31 |
| 0.48 | `C:\ProgramData\Kaspersky Lab\AVP21.26\Bases\Cache` | (no area) |  | 2026-10-31 |
| 0.79 | `C:\ProgramData\Kaspersky Lab\AVP21.26\Data` | (no area) |  | 2026-11-01 |
| 0.39 | `C:\ProgramData\Kaspersky Lab\AVP21.26\Data\webview2\Win10` | (no area) |  | 2026-08-06 |
| 0.77 | `C:\ProgramData\Kaspersky Lab\KSDE5.26` | (no area) |  | 2026-09-30 |
| 0.39 | `C:\ProgramData\Kaspersky Lab\KSDE5.26\Data\webview2\Win10` | (no area) |  | 2026-08-04 |
| 0.26 | `C:\ProgramData\Kaspersky Lab\KSDE5.26\Temp\Setup` | (no area) |  | 2026-10-29 |
| 2.27 | `C:\ProgramData\Adobe\CameraRaw` | (no area) |  | 2026-10-04 |
| 0.93 | `C:\ProgramData\Adobe\CameraRaw\ModelZoo` | (no area) |  | 2026-10-04 |
| 0.85 | `C:\ProgramData\Adobe\CameraRaw\CameraProfiles` | (no area) |  | 2026-10-04 |
| 0.70 | `C:\ProgramData\Adobe\CameraRaw\CameraProfiles\Camera` | (no area) |  | 2026-10-04 |
| 0.45 | `C:\ProgramData\Adobe\CameraRaw\LensProfiles\1.0` | (no area) |  | 2026-10-04 |
| 1.92 | `C:\ProgramData\Corsair\CUE5\GameSDKEffects` | (no area) |  | 2023-12-02 |
| 1.12 | `C:\ProgramData\Corsair\CUE5\GameSDKEffects\Common` | (no area) |  | 2023-12-02 |
| 1.63 | `C:\ProgramData\Epic\EpicGamesLauncher` | (no area) |  | 2025-03-14 |
| 1.50 | `C:\ProgramData\Epic\EpicGamesLauncher\Data` | (no area) |  | 2026-10-11 |
| 1.20 | `C:\ProgramData\Epic\EpicGamesLauncher\Data\ContentCache` | (no area) |  | 2026-10-12 |
| 1.55 | `C:\ProgramData\Reallusion` | (no area) |  | 2026-09-30 |
| 1.04 | `C:\ProgramData\Reallusion\ContentManager` | (no area) |  | 2024-11-09 |
| 0.91 | `C:\ProgramData\Reallusion\ContentManager\MetaCache` | (no area) |  | 2023-10-22 |
| 0.61 | `C:\ProgramData\Reallusion\ContentManager\MetaCache\Character Creator 4` | (no area) |  | 2025-05-14 |
| 0.31 | `C:\ProgramData\Reallusion\CMS` | (no area) |  | 2025-06-12 |
| 0.87 | `C:\ProgramData\ASUS` | (no area) |  | 2026-02-02 |
| 0.54 | `C:\ProgramData\ASUS\ARMOURY CRATE Diagnosis` | (no area) |  | 2024-12-25 |
| 0.39 | `C:\ProgramData\ASUS\ARMOURY CRATE Diagnosis\MBLedSDK` | (no area) |  | 2024-11-16 |
| 0.31 | `C:\ProgramData\ASUS\FestsEffect\data` | (no area) |  | 2024-11-16 |
| 0.39 | `C:\ProgramData\Razer` | (no area) |  | 2021-12-14 |
| 0.27 | `C:\ProgramData\Razer\Synapse3\Service` | (no area) |  | 2021-12-14 |
| 0.34 | `C:\ProgramData\Sony\Imaging Edge\OnnxData` | (no area) |  | 2026-04-21 |

## C:\Users\Public

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 34.86 | `C:\Users\Public` | (no area) |  | 2026-09-30 |
| 34.84 | `C:\Users\Public\Documents` | windows/documents | manual-review | 2026-09-30 |
| 18.34 | `C:\Users\Public\Documents\ZBrushData2025` | windows/documents | manual-review | 2025-05-18 |
| 17.59 | `C:\Users\Public\Documents\ZBrushData2025\QuickSave` | windows/documents | manual-review | 2025-07-02 |
| 0.66 | `C:\Users\Public\Documents\ZBrushData2025\ZPluginData\FBXExportImportData` | windows/documents | manual-review | 2025-07-02 |
| 15.04 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates` | windows/documents | manual-review | 2025-01-15 |
| 10.25 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D` | windows/documents | manual-review | 2025-01-15 |
| 3.26 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Characters` | windows/documents | manual-review | 2025-05-14 |
| 0.92 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Characters\Clothed` | windows/documents | manual-review | 2025-05-14 |
| 0.57 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Characters\Base` | windows/documents | manual-review | 2025-05-14 |
| 0.46 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Characters\Stylized` | windows/documents | manual-review | 2025-05-14 |
| 0.29 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Characters\4K` | windows/documents | manual-review | 2025-05-14 |
| 1.23 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\CC4 Projects` | windows/documents | manual-review | 2025-05-14 |
| 1.10 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Hair Builder` | windows/documents | manual-review | 2025-01-15 |
| 0.95 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Skin & Makeup` | windows/documents | manual-review | 2025-05-14 |
| 0.50 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Skin & Makeup\Normal` | windows/documents | manual-review | 2025-01-15 |
| 0.73 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Props` | windows/documents | manual-review | 2024-02-12 |
| 0.65 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Actor Group` | windows/documents | manual-review | 2024-08-10 |
| 0.39 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Actor Group\Actor Group` | windows/documents | manual-review | 2024-08-10 |
| 0.51 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Scenes & Effects` | windows/documents | manual-review | 2023-09-11 |
| 0.47 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Beard & Brows Builder` | windows/documents | manual-review | 2025-01-15 |
| 0.38 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Beard & Brows Builder\Beard` | windows/documents | manual-review | 2025-01-15 |
| 0.36 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Animation` | windows/documents | manual-review | 2025-01-15 |
| 0.26 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Reallusion 3D\Scan Reality - Sandra\Scan Reality` | windows/documents | manual-review | 2025-01-15 |
| 2.84 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Slayerazazel\Historical - Roman Male Commoners` | windows/documents | manual-review | 2025-01-02 |
| 0.65 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\3d pixel paul` | windows/documents | manual-review | 2025-01-15 |
| 0.33 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\3d pixel paul\High Quality Shadow Knight Armor` | windows/documents | manual-review | 2025-01-15 |
| 0.32 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\3d pixel paul\Rogue Armor\Rogue` | windows/documents | manual-review | 2024-02-12 |
| 0.50 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Xurge` | windows/documents | manual-review | 2025-01-15 |
| 0.32 | `C:\Users\Public\Documents\Reallusion\Reallusion Templates\Unknown Author\Single Items` | windows/documents | manual-review | 2025-05-28 |
| 1.47 | `C:\Users\Public\Documents\MarvelousDesigner\Assets` | windows/documents | manual-review | 2024-02-01 |

## ~

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 410.97 | `~` | (no area) |  | 2026-11-01 |
| 311.13 | `~\AppData` | (no area) |  | 2026-10-30 |
| 23.39 | `~\.codex` | ai-tools/assistant-homes | manual-review | 2026-11-01 |
| 17.94 | `~\.codex\sessions\2026\10` | ai-tools/codex-sessions | manual-review | 2026-10-30 |
| 2.67 | `~\.codex\sessions\2026\10\15` | ai-tools/codex-sessions | manual-review | 2026-10-16 |
| 2.60 | `~\.codex\sessions\2026\10\07` | ai-tools/codex-sessions | manual-review | 2026-10-10 |
| 2.15 | `~\.codex\sessions\2026\10\04` | ai-tools/codex-sessions | manual-review | 2026-10-07 |
| 1.41 | `~\.codex\sessions\2026\10\25` | ai-tools/codex-sessions | manual-review | 2026-10-25 |
| 1.35 | `~\.codex\sessions\2026\10\27` | ai-tools/codex-sessions | manual-review | 2026-10-27 |
| 1.29 | `~\.codex\sessions\2026\10\13` | ai-tools/codex-sessions | manual-review | 2026-10-16 |
| 1.16 | `~\.codex\sessions\2026\10\26` | ai-tools/codex-sessions | manual-review | 2026-10-26 |
| 0.87 | `~\.codex\sessions\2026\10\11` | ai-tools/codex-sessions | manual-review | 2026-10-12 |
| 0.67 | `~\.codex\sessions\2026\10\24` | ai-tools/codex-sessions | manual-review | 2026-10-25 |
| 0.55 | `~\.codex\sessions\2026\10\10` | ai-tools/codex-sessions | manual-review | 2026-10-12 |
| 0.52 | `~\.codex\sessions\2026\10\12` | ai-tools/codex-sessions | manual-review | 2026-10-14 |
| 0.39 | `~\.codex\sessions\2026\10\19` | ai-tools/codex-sessions | manual-review | 2026-10-22 |
| 0.35 | `~\.codex\sessions\2026\10\08` | ai-tools/codex-sessions | manual-review | 2026-10-09 |
| 0.34 | `~\.codex\sessions\2026\10\14` | ai-tools/codex-sessions | manual-review | 2026-10-16 |
| 0.26 | `~\.codex\sessions\2026\10\18` | ai-tools/codex-sessions | manual-review | 2026-10-19 |
| 0.26 | `~\.codex\sessions\2026\10\22` | ai-tools/codex-sessions | manual-review | 2026-10-23 |
| 1.33 | `~\.codex\generated_images` | ai-tools/codex-images | manual-review | 2026-10-27 |
| 0.60 | `~\.codex\archived_sessions` | ai-tools/codex-sessions | manual-review | 2026-10-04 |
| 0.45 | `~\.codex\plugins` | ai-tools/assistant-homes | manual-review | 2026-10-07 |
| 0.40 | `~\.codex\plugins\.plugin-appserver` | ai-tools/assistant-homes | manual-review | 2026-10-07 |
| 0.34 | `~\.codex\.sandbox-bin` | ai-tools/assistant-homes | manual-review | 2026-10-26 |
| 11.44 | `~\.rustup` | rust/toolchains | relocate | 2025-10-02 |
| 10.93 | `~\.rustup\toolchains` | rust/toolchains | relocate | 2026-10-24 |
| 2.22 | `~\.rustup\toolchains\nightly-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-07-23 |
| 1.06 | `~\.rustup\toolchains\nightly-x86_64-pc-windows-msvc\lib\rustlib` | rust/toolchains | relocate | 2026-07-23 |
| 0.60 | `~\.rustup\toolchains\nightly-x86_64-pc-windows-msvc\lib\rustlib\x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-07-23 |
| 0.34 | `~\.rustup\toolchains\nightly-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-07-23 |
| 2.10 | `~\.rustup\toolchains\stable-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-07-19 |
| 0.92 | `~\.rustup\toolchains\stable-x86_64-pc-windows-msvc\lib\rustlib` | rust/toolchains | relocate | 2026-07-19 |
| 0.59 | `~\.rustup\toolchains\stable-x86_64-pc-windows-msvc\lib\rustlib\x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-07-19 |
| 0.38 | `~\.rustup\toolchains\stable-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-07-19 |
| 0.95 | `~\.rustup\toolchains\1.93.1-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-05-09 |
| 0.61 | `~\.rustup\toolchains\1.93.1-x86_64-pc-windows-msvc\lib\rustlib` | rust/toolchains | relocate | 2026-05-09 |
| 0.56 | `~\.rustup\toolchains\1.93.1-x86_64-pc-windows-msvc\lib\rustlib\x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-05-09 |
| 0.32 | `~\.rustup\toolchains\1.93.1-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-05-09 |
| 0.95 | `~\.rustup\toolchains\nightly-2026-07-23-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-10-13 |
| 0.60 | `~\.rustup\toolchains\nightly-2026-07-23-x86_64-pc-windows-msvc\lib\rustlib\x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-10-13 |
| 0.33 | `~\.rustup\toolchains\nightly-2026-07-23-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-10-13 |
| 0.90 | `~\.rustup\toolchains\1.93.0-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-09-18 |
| 0.56 | `~\.rustup\toolchains\1.93.0-x86_64-pc-windows-msvc\lib\rustlib\x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-09-18 |
| 0.32 | `~\.rustup\toolchains\1.93.0-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-09-18 |
| 0.64 | `~\.rustup\toolchains\nightly-2026-02-27-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-07-14 |
| 0.33 | `~\.rustup\toolchains\nightly-2026-02-27-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-07-14 |
| 0.29 | `~\.rustup\toolchains\nightly-2026-02-27-x86_64-pc-windows-msvc\lib\rustlib` | rust/toolchains | relocate | 2026-07-14 |
| 0.60 | `~\.rustup\toolchains\1.98.0-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-09-18 |
| 0.33 | `~\.rustup\toolchains\1.98.0-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-09-18 |
| 0.25 | `~\.rustup\toolchains\1.98.0-x86_64-pc-windows-msvc\lib\rustlib\x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-09-18 |
| 0.60 | `~\.rustup\toolchains\1.99.0-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-10-24 |
| 0.31 | `~\.rustup\toolchains\1.99.0-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-10-24 |
| 0.26 | `~\.rustup\toolchains\1.99.0-x86_64-pc-windows-msvc\lib\rustlib\x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-10-24 |
| 0.54 | `~\.rustup\toolchains\1.88-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-07-19 |
| 0.29 | `~\.rustup\toolchains\1.88-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-07-19 |
| 0.53 | `~\.rustup\toolchains\1.87-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-07-14 |
| 0.28 | `~\.rustup\toolchains\1.87-x86_64-pc-windows-msvc\bin` | rust/toolchains | relocate | 2026-07-14 |
| 0.47 | `~\.rustup\toolchains\1.93-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-09-25 |
| 0.43 | `~\.rustup\toolchains\1.93-x86_64-pc-windows-msvc\lib\rustlib\x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2026-09-25 |
| 0.43 | `~\.rustup\toolchains\nightly-2025-01-01-x86_64-pc-windows-msvc` | rust/toolchains | relocate | 2025-10-12 |
| 0.40 | `~\.rustup\tmp` | rust/toolchains | relocate | 2026-10-24 |
| 11.35 | `~\.elyriu-local-ai` | ai-tools/model-tool-homes | manual-review | 2026-06-25 |
| 6.40 | `~\.elyriu-local-ai\models` | ai-tools/local-ai-models | manual-review | 2026-06-25 |
| 3.95 | `~\.elyriu-local-ai\models\models--Qwen--Qwen-Image-Edit\blobs` | ai-tools/local-ai-models | manual-review | 2026-06-25 |
| 2.44 | `~\.elyriu-local-ai\models\models--HiDream-ai--HiDream-I1-Dev` | ai-tools/local-ai-models | manual-review | 2026-06-25 |
| 1.97 | `~\.elyriu-local-ai\models\models--HiDream-ai--HiDream-I1-Dev\blobs` | ai-tools/local-ai-models | manual-review | 2026-06-25 |
| 0.46 | `~\.elyriu-local-ai\models\models--HiDream-ai--HiDream-I1-Dev\snapshots\0fad2ea0ccf9a80ddf019ea777eedb27c1ccb232\text_encoder` | ai-tools/local-ai-models | manual-review | 2026-06-25 |
| 4.95 | `~\.elyriu-local-ai\runtime\Lib\site-packages` | ai-tools/model-tool-homes | manual-review | 2026-06-25 |
| 4.26 | `~\.elyriu-local-ai\runtime\Lib\site-packages\torch\lib` | ai-tools/model-tool-homes | manual-review | 2026-06-25 |
| 9.22 | `~\Documents` | windows/documents | manual-review | 2026-10-10 |
| 3.20 | `~\Documents\IK Multimedia\IK Product Manager\AmpliTube 5` | windows/documents | manual-review | 2026-05-16 |
| 3.06 | `~\Documents\GoXLR\Samples\Recorded` | windows/documents | manual-review | 2026-02-11 |
| 1.40 | `~\Documents\InstaLOD Studio\Exports` | windows/documents | manual-review | 2026-10-17 |
| 0.68 | `~\Documents\Gravações de som` | windows/documents | manual-review | 2025-07-26 |
| 0.44 | `~\Documents\AutoRigPro\Armatures Presets` | windows/documents | manual-review | 2025-06-11 |
| 8.80 | `~\Downloads` | windows/downloads | manual-review | 2026-10-29 |
| 7.15 | `~\.gradle` | java-gradle/gradle | relocate | 2026-10-30 |
| 6.41 | `~\.gradle\caches` | java-gradle/gradle | relocate | 2026-10-30 |
| 4.74 | `~\.gradle\caches\9.3.1\transforms` | java-gradle/gradle | relocate | 2026-11-01 |
| 1.46 | `~\.gradle\caches\modules-2\files-2.1` | java-gradle/gradle | relocate | 2026-11-01 |
| 0.51 | `~\.gradle\caches\modules-2\files-2.1\io.flutter` | java-gradle/gradle | relocate | 2026-10-31 |
| 0.40 | `~\.gradle\caches\modules-2\files-2.1\org.jetbrains.kotlin` | java-gradle/gradle | relocate | 2026-10-31 |
| 0.73 | `~\.gradle\wrapper\dists\gradle-9.3.1-all\9ot9r568e8zfvvd4mn8rbu1j0` | java-gradle/gradle | relocate | 2026-10-30 |
| 0.51 | `~\.gradle\wrapper\dists\gradle-9.3.1-all\9ot9r568e8zfvvd4mn8rbu1j0\gradle-9.3.1` | java-gradle/gradle | relocate | 2026-10-30 |
| 6.79 | `~\.claude` | ai-tools/assistant-homes | manual-review | 2026-10-31 |
| 6.63 | `~\.claude\projects` | ai-tools/claude-projects | manual-review | 2026-10-31 |
| 2.64 | `~\.claude\projects\e--UzmiGames-Assets3D` | ai-tools/claude-projects | manual-review | 2026-10-29 |
| 0.30 | `~\.claude\projects\e--UzmiGames-Assets3D\a33ab4c8-ba79-4270-87ca-3270e9bad9b9\subagents` | ai-tools/claude-projects | manual-review | 2026-10-08 |
| 1.92 | `~\.claude\projects\e--UzmiGames-UzPainter` | ai-tools/claude-projects | manual-review | 2026-10-24 |
| 1.33 | `~\.claude\projects\e--UzmiGames-UzPainter\06e8fc50-d3c2-4df6-bf29-0b84e80e4915` | ai-tools/claude-projects | manual-review | 2026-10-13 |
| 1.20 | `~\.claude\projects\e--UzmiGames-UzPainter\06e8fc50-d3c2-4df6-bf29-0b84e80e4915\subagents` | ai-tools/claude-projects | manual-review | 2026-10-22 |
| 0.48 | `~\.claude\projects\d--V3-ar-v3-dashboard` | ai-tools/claude-projects | manual-review | 2026-11-01 |
| 0.44 | `~\.claude\projects\f--Node-auditoria-2turno-eleicao2026` | ai-tools/claude-projects | manual-review | 2026-10-30 |
| 3.00 | `~\.cache` | dev-tools/user-cache | manual-review | 2026-10-04 |
| 1.56 | `~\.cache\ldbc-snb` | dev-tools/user-cache | manual-review | 2026-07-19 |
| 1.16 | `~\.cache\ldbc-snb\sf1` | dev-tools/user-cache | manual-review | 2026-07-19 |
| 0.87 | `~\.cache\ldbc-snb\sf1\social_network-sf1-CsvCompositeMergeForeign-LongDateFormatter` | dev-tools/user-cache | manual-review | 2023-08-18 |
| 0.58 | `~\.cache\ldbc-snb\sf1\social_network-sf1-CsvCompositeMergeForeign-LongDateFormatter\dynamic` | dev-tools/user-cache | manual-review | 2023-08-18 |
| 0.28 | `~\.cache\ldbc-snb\sf1\social_network-sf1-numpart-1` | dev-tools/user-cache | manual-review | 2023-05-17 |
| 0.29 | `~\.cache\ldbc-snb\archives` | dev-tools/user-cache | manual-review | 2026-07-19 |
| 1.33 | `~\.cache\codex-runtimes\codex-primary-runtime\dependencies` | dev-tools/user-cache | manual-review | 2026-10-07 |
| 0.47 | `~\.cache\codex-runtimes\codex-primary-runtime\dependencies\native` | dev-tools/user-cache | manual-review | 2026-10-07 |
| 0.44 | `~\.cache\codex-runtimes\codex-primary-runtime\dependencies\python` | dev-tools/user-cache | manual-review | 2026-10-07 |
| 0.42 | `~\.cache\codex-runtimes\codex-primary-runtime\dependencies\node` | dev-tools/user-cache | manual-review | 2026-10-07 |
| 0.33 | `~\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\node_modules` | dev-tools/user-cache | manual-review | 2026-10-07 |
| 2.76 | `~\.android` | android/user-home | manual-review | 2026-10-30 |
| 2.50 | `~\.android\avd\ar_pixel.avd\snapshots\default_boot` | android/avd | manual-review | 2026-10-30 |
| 2.51 | `~\.cortex` | (no area) |  | 2026-10-18 |
| 1.82 | `~\.cortex\local` | (no area) |  | 2026-11-01 |
| 0.43 | `~\.cortex\local\segments` | (no area) |  | 2026-10-17 |
| 0.48 | `~\.cortex\archive\events` | (no area) |  | 2026-04-30 |
| 0.45 | `~\.cortex\archive\events\year=2026` | (no area) |  | 2026-10-01 |
| 2.37 | `~\Desktop` | windows/documents | manual-review | 2026-10-31 |
| 1.90 | `~\Desktop\Ar Online` | windows/documents | manual-review | 2026-10-27 |
| 0.36 | `~\Desktop\Ar Online\bikeexpedition` | windows/documents | manual-review | 2026-05-14 |
| 0.31 | `~\Desktop\Ar Online\bikeexpedition\wordpress\2021` | windows/documents | manual-review | 2026-05-14 |
| 0.45 | `~\Desktop\<name>\2024` | windows/documents | manual-review | 2026-06-10 |
| 2.33 | `~\.vscode\extensions` | vscode/editor-extensions | manual-review | 2026-10-30 |
| 1.57 | `~\.vscode\extensions\openai.chatgpt-26.1021.22027-win32-x64` | vscode/editor-extensions | manual-review | 2026-10-23 |
| 0.90 | `~\.vscode\extensions\openai.chatgpt-26.1021.22027-win32-x64\bin` | vscode/editor-extensions | manual-review | 2026-10-23 |
| 0.48 | `~\.vscode\extensions\openai.chatgpt-26.1021.22027-win32-x64\bin\windows-x86_64` | vscode/editor-extensions | manual-review | 2026-10-23 |
| 0.42 | `~\.vscode\extensions\openai.chatgpt-26.1021.22027-win32-x64\bin\linux-x86_64` | vscode/editor-extensions | manual-review | 2026-10-23 |
| 0.66 | `~\.vscode\extensions\openai.chatgpt-26.1021.22027-win32-x64\webview\assets` | vscode/editor-extensions | manual-review | 2026-10-23 |
| 0.28 | `~\.vscode\extensions\anthropic.claude-code-2.1.314-win32-x64\resources\native-binary` | vscode/editor-extensions | manual-review | 2026-10-30 |
| 0.28 | `~\.vscode\extensions\anthropic.claude-code-2.1.312-win32-x64\resources\native-binary` | vscode/editor-extensions | manual-review | 2026-10-28 |
| 2.07 | `~\.cargo` | rust/cargo-home | relocate | 2026-11-01 |
| 1.62 | `~\.cargo\registry` | rust/cargo-registry | clean-rule | 2025-07-07 |
| 1.26 | `~\.cargo\registry\src\index.crates.io-1949cf8c6b5b557f` | rust/cargo-registry | clean-rule | 2026-11-01 |
| 0.26 | `~\.cargo\registry\cache\index.crates.io-1949cf8c6b5b557f` | rust/cargo-registry | clean-rule | 2026-11-01 |
| 0.27 | `~\.cargo\bin` | rust/cargo-home | relocate | 2026-10-24 |
| 1.21 | `~\.bun` | (no area) |  | 2026-02-10 |
| 1.11 | `~\.bun\install` | (no area) |  | 2026-06-29 |
| 1.11 | `~\.bun\install\cache` | node/js-runtime-caches | manual-review | 2026-04-06 |
| 1.09 | `~\.local` | (no area) |  | 2026-04-28 |
| 0.69 | `~\.local\share\claude\versions` | (no area) |  | 2026-10-06 |
| 0.37 | `~\.local\bin` | (no area) |  | 2026-10-14 |
| 0.58 | `~\.docker` | (no area) |  | 2026-11-01 |
| 0.47 | `~\.docker\scout\sbom\sha256` | (no area) |  | 2026-11-01 |
| 0.50 | `~\neo4j\data\transactions` | (no area) |  | 2025-02-18 |
| 0.25 | `~\neo4j\data\transactions\neo4j` | (no area) |  | 2025-02-18 |
| 0.25 | `~\neo4j\data\transactions\system` | (no area) |  | 2025-02-18 |
| 0.49 | `~\.git\lfs\objects` | (no area) |  | 2025-10-17 |
| 0.43 | `~\.3T` | (no area) |  | 2025-10-02 |
| 0.26 | `~\.3T\updates` | (no area) |  | 2024-07-14 |
| 0.29 | `~\Ubiquiti UniFi\data\db\journal` | (no area) |  | 2019-11-28 |

## ~\AppData\Local

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 276.38 | `~\AppData\Local` | (no area) |  | 2026-11-01 |
| 136.13 | `~\AppData\Local\Packages` | windows/store-app-data | manual-review | 2026-11-01 |
| 125.19 | `~\AppData\Local\Packages\CanonicalGroupLimited.Ubuntu24.04LTS_79rhkp1fndgsc\LocalState` | windows/store-app-data | manual-review | 2026-10-31 |
| 125.19 | `~\AppData\Local\Packages\CanonicalGroupLimited.Ubuntu24.04LTS_79rhkp1fndgsc\LocalState\rootfs` | wsl-docker/distro-files | relocate | 2026-10-29 |
| 6.93 | `~\AppData\Local\Packages\PythonSoftwareFoundation.Python.3.12_qbz5n2kfra8p0\LocalCache` | python-store/installs | uninstall | 2026-09-30 |
| 0.87 | `~\AppData\Local\Packages\OpenAI.Codex_2p2nqsd0c76g0\LocalCache` | windows/store-app-data | manual-review | 2026-10-04 |
| 46.65 | `~\AppData\Local\UzPainter` | uzpainter/app-data | manual-review | 2026-10-26 |
| 30.86 | `~\AppData\Local\UzPainter\oracle-work` | uzpainter/app-data | manual-review | 2026-10-26 |
| 12.69 | `~\AppData\Local\UzPainter\oracle-work\envelopes` | uzpainter/app-data | manual-review | 2026-10-23 |
| 0.97 | `~\AppData\Local\UzPainter\oracle-work\phase2__tier1__tunic-v10\20261013T042210906855Z-92b58e0185e7` | uzpainter/app-data | manual-review | 2026-10-13 |
| 0.75 | `~\AppData\Local\UzPainter\oracle-work\codex-20261020` | uzpainter/app-data | manual-review | 2026-10-20 |
| 0.49 | `~\AppData\Local\UzPainter\oracle-work\tunic-base-mesh-control\run-fdv1llk3` | uzpainter/app-data | manual-review | 2026-10-23 |
| 0.46 | `~\AppData\Local\UzPainter\oracle-work\tier0__semantics__virtual-maps-coverage-float` | uzpainter/app-data | manual-review | 2026-10-12 |
| 0.40 | `~\AppData\Local\UzPainter\oracle-work\tier0__semantics__export-resize\20261012T044641692990Z-9da068951e19` | uzpainter/app-data | manual-review | 2026-10-12 |
| 0.33 | `~\AppData\Local\UzPainter\oracle-work\tier0__semantics__export-padding-kernel-tie-2048\20261019T125949265228Z-8ed8499667f2` | uzpainter/app-data | manual-review | 2026-10-19 |
| 0.32 | `~\AppData\Local\UzPainter\oracle-work\tier0__defaults-templates__project-templates\20261010T081811515536Z-26e2dd78c0d6` | uzpainter/app-data | manual-review | 2026-10-10 |
| 0.26 | `~\AppData\Local\UzPainter\oracle-work\post-refresh-probes` | uzpainter/app-data | manual-review | 2026-10-23 |
| 0.25 | `~\AppData\Local\UzPainter\oracle-work\tier0__semantics__export-padding-kernel-atlas-2048\20261019T125939736598Z-8ed8499667f2` | uzpainter/app-data | manual-review | 2026-10-19 |
| 9.48 | `~\AppData\Local\UzPainter\verification\ef618d436ee5\pytest-temp` | uzpainter/app-data | manual-review | 2026-10-23 |
| 2.90 | `~\AppData\Local\UzPainter\oracle-inbox` | uzpainter/app-data | manual-review | 2026-10-26 |
| 1.31 | `~\AppData\Local\UzPainter\oracle-inbox\envelopes` | uzpainter/app-data | manual-review | 2026-10-23 |
| 1.17 | `~\AppData\Local\UzPainter\oracle-inbox\envelopes\painter-12.2.0_js-api.1.1.20-env-a981b586eae7` | uzpainter/app-data | manual-review | 2026-10-23 |
| 1.86 | `~\AppData\Local\UzPainter\parity-store\blobs` | uzpainter/app-data | manual-review | 2026-10-11 |
| 0.38 | `~\AppData\Local\UzPainter\parity-store\blobs\c4` | uzpainter/app-data | manual-review | 2026-10-23 |
| 1.23 | `~\AppData\Local\UzPainter\cache\eval` | uzpainter/cache | manual-review | 2026-10-23 |
| 21.90 | `~\AppData\Local\Google\Chrome` | (no area) |  | 2021-12-14 |
| 21.90 | `~\AppData\Local\Google\Chrome\User Data` | browsers/profiles | manual-review | 2026-11-01 |
| 16.61 | `~\AppData\Local\Google\Chrome\User Data\Default` | browsers/profiles | manual-review | 2026-11-01 |
| 0.31 | `~\AppData\Local\Google\Chrome\User Data\Snapshots` | browsers/profiles | manual-review | 2026-09-30 |
| 14.25 | `~\AppData\Local\Temp` | windows/user-temp | clean-rule | 2026-11-01 |
| 6.54 | `~\AppData\Local\Temp\6C648F19-5F87-4B13-919C-67F23F5D11CD` | windows/user-temp | clean-rule | 2026-10-10 |
| 3.84 | `~\AppData\Local\Temp\qpgzmvzm` | windows/user-temp | clean-rule | 2026-10-29 |
| 0.55 | `~\AppData\Local\Temp\qpgzmvzm\Win11SDK_10.0.22621.87E8503273CF7FD41683\Installers` | windows/user-temp | clean-rule | 2026-10-29 |
| 0.49 | `~\AppData\Local\Temp\qpgzmvzm\Win11SDK_10.0.26100.F872955055CDB6106FAD\Installers` | windows/user-temp | clean-rule | 2026-10-29 |
| 0.31 | `~\AppData\Local\Temp\qpgzmvzm\Win11SDK_WindowsPerformanceToolkit.45441DD1347DD4DE327E\Installers` | windows/user-temp | clean-rule | 2026-10-29 |
| 0.98 | `~\AppData\Local\Temp\claude` | windows/user-temp | clean-rule | 2026-11-01 |
| 0.75 | `~\AppData\Local\Temp\claude\f--Node-auditoria-2turno-eleicao2026` | windows/user-temp | clean-rule | 2026-10-31 |
| 0.73 | `~\AppData\Local\Temp\DockerDesktop\ajqkf5g0xr4` | windows/user-temp | clean-rule | 2026-10-24 |
| 0.62 | `~\AppData\Local\Temp\DockerDesktop\ajqkf5g0xr4\resources` | windows/user-temp | clean-rule | 2026-10-22 |
| 8.15 | `~\AppData\Local\Android\Sdk` | android/sdk | manual-review | 2026-10-30 |
| 1.05 | `~\AppData\Local\Android\Sdk\emulator` | android/sdk | manual-review | 2026-10-30 |
| 0.41 | `~\AppData\Local\Android\Sdk\emulator\lib64` | android/sdk | manual-review | 2026-10-30 |
| 0.35 | `~\AppData\Local\Android\Sdk\platforms` | android/sdk | manual-review | 2026-10-31 |
| 6.98 | `~\AppData\Local\Microsoft` | (no area) |  | 2026-10-15 |
| 2.19 | `~\AppData\Local\Microsoft\OneDrive` | (no area) |  | 2026-10-28 |
| 0.53 | `~\AppData\Local\Microsoft\OneDrive\26.189.0928.0009` | (no area) |  | 2026-10-28 |
| 1.88 | `~\AppData\Local\Microsoft\PowerToys` | (no area) |  | 2022-01-06 |
| 1.83 | `~\AppData\Local\Microsoft\PowerToys\Updates` | windows/powertoys-updates | manual-review | 2026-10-28 |
| 1.30 | `~\AppData\Local\Microsoft\VisualStudio` | visual-studio/user | manual-review | 2026-02-25 |
| 0.90 | `~\AppData\Local\Microsoft\VisualStudio\Roslyn\Cache` | visual-studio/user | manual-review | 2025-07-04 |
| 0.67 | `~\AppData\Local\Microsoft\Edge` | (no area) |  | 2021-12-14 |
| 0.67 | `~\AppData\Local\Microsoft\Edge\User Data` | browsers/profiles | manual-review | 2026-10-24 |
| 0.46 | `~\AppData\Local\Microsoft\WinGet\Packages` | (no area) |  | 2026-09-17 |
| 0.36 | `~\AppData\Local\Microsoft\Windows` | (no area) |  | 2026-10-26 |
| 3.29 | `~\AppData\Local\ms-playwright-mcp` | playwright/mcp-profiles | manual-review | 2026-10-30 |
| 0.95 | `~\AppData\Local\ms-playwright-mcp\mcp-chrome-f07e86a\Default` | playwright/mcp-profiles | manual-review | 2026-07-08 |
| 0.55 | `~\AppData\Local\ms-playwright-mcp\mcp-chrome-86e9a4b\Default` | playwright/mcp-profiles | manual-review | 2026-09-20 |
| 0.40 | `~\AppData\Local\ms-playwright-mcp\mcp-chrome-22a2426` | playwright/mcp-profiles | manual-review | 2026-11-01 |
| 0.38 | `~\AppData\Local\ms-playwright-mcp\mcp-chrome-22a2426\Default` | playwright/mcp-profiles | manual-review | 2026-11-01 |
| 0.34 | `~\AppData\Local\ms-playwright-mcp\mcp-chrome-0af9a06\Default` | playwright/mcp-profiles | manual-review | 2026-10-20 |
| 2.80 | `~\AppData\Local\Programs` | windows/user-programs | uninstall | 2025-10-13 |
| 0.86 | `~\AppData\Local\Programs\CD Projekt Red\REDlauncher` | windows/user-programs | uninstall | 2024-07-06 |
| 2.66 | `~\AppData\Local\UnrealEngine` | unreal/user | manual-review | 2026-10-11 |
| 2.37 | `~\AppData\Local\UnrealEngine\Common` | unreal/user | manual-review | 2025-11-13 |
| 1.52 | `~\AppData\Local\UnrealEngine\Common\Zen\Data` | unreal/user | manual-review | 2026-10-25 |
| 0.78 | `~\AppData\Local\UnrealEngine\Common\DerivedDataCache` | unreal/ddc | relocate | 2026-10-11 |
| 0.65 | `~\AppData\Local\UnrealEngine\Common\DerivedDataCache\Content` | unreal/ddc | relocate | 2026-10-12 |
| 0.28 | `~\AppData\Local\UnrealEngine\5.7` | unreal/user | manual-review | 2025-11-22 |
| 1.98 | `~\AppData\Local\vcpkg` | (no area) |  | 2024-08-07 |
| 1.73 | `~\AppData\Local\vcpkg\downloads` | (no area) |  | 2024-08-06 |
| 1.31 | `~\AppData\Local\vcpkg\downloads\tools` | (no area) |  | 2024-08-06 |
| 1.97 | `~\AppData\Local\NVIDIA` | (no area) |  | 2026-10-11 |
| 1.77 | `~\AppData\Local\NVIDIA\DXCache` | gpu/nvidia-dxcache | clean-rule | 2026-11-01 |
| 1.63 | `~\AppData\Local\Discord` | windows/user-programs | uninstall | 2026-09-30 |
| 0.44 | `~\AppData\Local\Discord\app-1.0.9260` | windows/user-programs | uninstall | 2026-09-30 |
| 0.44 | `~\AppData\Local\Discord\app-1.0.9259` | windows/user-programs | uninstall | 2026-09-23 |
| 0.44 | `~\AppData\Local\Discord\app-1.0.9258` | windows/user-programs | uninstall | 2026-09-30 |
| 1.59 | `~\AppData\Local\electron` | (no area) |  | 2025-09-18 |
| 1.59 | `~\AppData\Local\electron\Cache` | node/js-runtime-caches | manual-review | 2026-05-01 |
| 1.55 | `~\AppData\Local\npm-cache` | node/npm-cache | relocate | 2026-10-29 |
| 1.13 | `~\AppData\Local\npm-cache\_npx` | node/npm-cache | relocate | 2026-10-29 |
| 0.42 | `~\AppData\Local\npm-cache\_cacache\content-v2\sha512` | node/npm-cache | relocate | 2026-10-30 |
| 1.45 | `~\AppData\Local\java-db` | (no area) |  | 2026-10-19 |
| 1.45 | `~\AppData\Local\ms-playwright` | playwright/browsers | clean-rule | 2026-11-01 |
| 0.42 | `~\AppData\Local\ms-playwright\chromium-1234\chrome-win64` | playwright/browsers | clean-rule | 2026-07-29 |
| 0.26 | `~\AppData\Local\ms-playwright\chromium_headless_shell-1234\chrome-headless-shell-win64` | playwright/browsers | clean-rule | 2026-08-09 |
| 1.24 | `~\AppData\Local\ort.pyke.io\dfbin\x86_64-pc-windows-msvc` | (no area) |  | 2026-08-29 |
| 0.33 | `~\AppData\Local\ort.pyke.io\dfbin\x86_64-pc-windows-msvc\f7c654b3729cb9e5ad2a36a0c38e5b48e63bf4eed22968931aed33a0ad0b527d` | (no area) |  | 2026-08-29 |
| 0.30 | `~\AppData\Local\ort.pyke.io\dfbin\x86_64-pc-windows-msvc\a03948a02dec0c38485febb0e54ccb89af070d3adcbcb91e2e4fa5fec278943a` | (no area) |  | 2026-04-17 |
| 0.30 | `~\AppData\Local\ort.pyke.io\dfbin\x86_64-pc-windows-msvc\b685bfc8d336e0ba95c066a7a982c03aa6dedd528a492eb99ca4ccb7f3af9e7a` | (no area) |  | 2026-04-05 |
| 1.16 | `~\AppData\Local\Reallusion` | (no area) |  | 2025-05-18 |
| 0.73 | `~\AppData\Local\Reallusion\RLDownloads` | (no area) |  | 2025-05-18 |
| 0.35 | `~\AppData\Local\Reallusion\ActorCore AccuRIG` | (no area) |  | 2026-04-17 |
| 0.98 | `~\AppData\Local\camoufox\camoufox\Cache` | (no area) |  | 2025-10-02 |
| 0.67 | `~\AppData\Local\camoufox\camoufox\Cache\fonts` | (no area) |  | 2025-09-21 |
| 0.84 | `~\AppData\Local\Adobe` | adobe/app-data | manual-review | 2026-10-22 |
| 0.31 | `~\AppData\Local\Adobe\Adobe Substance 3D Painter\cache` | adobe/app-data | manual-review | 2026-01-27 |
| 0.80 | `~\AppData\Local\MongoDBCompass` | (no area) |  | 2025-04-17 |
| 0.33 | `~\AppData\Local\MongoDBCompass\app-1.46.0` | (no area) |  | 2025-04-17 |
| 0.33 | `~\AppData\Local\MongoDBCompass\app-1.45.4` | (no area) |  | 2025-03-11 |
| 0.76 | `~\AppData\Local\Pub\Cache\hosted` | (no area) |  | 2026-10-20 |
| 0.68 | `~\AppData\Local\Pub\Cache\hosted\pub.dev` | (no area) |  | 2026-11-01 |
| 0.79 | `~\AppData\Local\pnpm` | node/pnpm-store | clean-rule | 2026-11-01 |
| 0.35 | `~\AppData\Local\pnpm\.tools\@pnpm+win-x64` | node/pnpm-store | clean-rule | 2026-10-30 |
| 0.33 | `~\AppData\Local\pnpm\store\v10\files` | node/pnpm-store | clean-rule | 2025-09-04 |
| 0.68 | `~\AppData\Local\OpenAI\Codex` | (no area) |  | 2026-10-04 |
| 0.40 | `~\AppData\Local\OpenAI\Codex\bin\9691020b546a15b2` | (no area) |  | 2026-10-07 |
| 0.27 | `~\AppData\Local\OpenAI\Codex\runtimes\cua_node` | (no area) |  | 2026-10-07 |
| 0.67 | `~\AppData\Local\Steam` | (no area) |  | 2026-10-16 |
| 0.63 | `~\AppData\Local\Steam\htmlcache` | (no area) |  | 2026-10-31 |
| 0.59 | `~\AppData\Local\Steam\htmlcache\Default` | (no area) |  | 2026-10-31 |
| 0.58 | `~\AppData\Local\b1\Saved` | (no area) |  | 2024-08-20 |
| 0.57 | `~\AppData\Local\ABInfinite\Saved` | (no area) |  | 2025-07-19 |
| 0.40 | `~\AppData\Local\ABInfinite\Saved\Logs` | (no area) |  | 2025-07-19 |
| 0.55 | `~\AppData\Local\lens-updater` | (no area) |  | 2021-12-24 |
| 0.31 | `~\AppData\Local\lens-updater\pending` | (no area) |  | 2025-10-02 |
| 0.52 | `~\AppData\Local\Wardogs\Saved` | (no area) |  | 2026-09-20 |
| 0.52 | `~\AppData\Local\lm-studio-updater` | (no area) |  | 2025-10-07 |
| 0.26 | `~\AppData\Local\lm-studio-updater\pending` | (no area) |  | 2025-10-08 |
| 0.50 | `~\AppData\Local\Path of Exile 2` | (no area) |  | 2026-09-05 |
| 0.29 | `~\AppData\Local\Path of Exile 2\Shaders` | (no area) |  | 2026-09-04 |
| 0.50 | `~\AppData\Local\UzEngine\logs` | (no area) |  | 2026-10-27 |
| 0.50 | `~\AppData\Local\Autodesk` | (no area) |  | 2026-03-13 |
| 0.28 | `~\AppData\Local\Autodesk\Identity Services\Log` | (no area) |  | 2026-04-06 |
| 0.49 | `~\AppData\Local\accurig-updater` | (no area) |  | 2026-04-17 |
| 0.48 | `~\AppData\Local\actorcore-accurig-updater` | (no area) |  | 2022-11-27 |
| 0.48 | `~\AppData\Local\nvm` | (no area) |  | 2026-10-30 |
| 0.47 | `~\AppData\Local\SCUM\Saved` | (no area) |  | 2025-07-20 |
| 0.37 | `~\AppData\Local\SCUM\Saved\Crashes` | (no area) |  | 2025-08-10 |
| 0.39 | `~\AppData\Local\CrashDumps` | windows/crash-dumps | clean-rule | 2026-10-31 |
| 0.36 | `~\AppData\Local\EpicGamesLauncher\Saved` | (no area) |  | 2025-03-14 |
| 0.29 | `~\AppData\Local\EpicGamesLauncher\Saved\webcache_4430` | games/epic-webcache | clean-rule | 2026-10-12 |
| 0.36 | `~\AppData\Local\TL\Saved` | (no area) |  | 2024-07-23 |
| 0.35 | `~\AppData\Local\enshrouded\caches` | (no area) |  | 2024-07-04 |
| 0.34 | `~\AppData\Local\ledger-live-desktop-updater` | (no area) |  | 2022-04-18 |
| 0.30 | `~\AppData\Local\ConnectedDevicesPlatform\8d54964dbcc47e45` | (no area) |  | 2021-12-14 |
| 0.31 | `~\AppData\Local\zig` | zig/global-cache | clean-rule | 2026-03-13 |
| 0.30 | `~\AppData\Local\.dartServer\.analysis-driver` | (no area) |  | 2026-11-01 |
| 0.28 | `~\AppData\Local\netmarblelauncher-updater` | (no area) |  | 2025-02-12 |
| 0.28 | `~\AppData\Local\binance-updater` | (no area) |  | 2021-12-23 |
| 0.26 | `~\AppData\Local\NVIDIA Corporation` | (no area) |  | 2026-03-09 |
| 0.25 | `~\AppData\Local\JxBrowser\7.22` | (no area) |  | 2025-10-02 |
| 0.25 | `~\AppData\Local\fanal` | (no area) |  | 2024-08-12 |

## ~\AppData\Roaming

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 32.84 | `~\AppData\Roaming` | (no area) |  | 2026-10-19 |
| 12.92 | `~\AppData\Roaming\Code` | (no area) |  | 2026-10-31 |
| 11.05 | `~\AppData\Roaming\Code\WebStorage` | vscode/web-storage | manual-review | 2026-10-05 |
| 0.94 | `~\AppData\Roaming\Code\WebStorage\9\CacheStorage` | vscode/web-storage | manual-review | 2026-10-31 |
| 1.06 | `~\AppData\Roaming\Code\CachedExtensionVSIXs` | vscode/vsix-cache | clean-rule | 2026-11-01 |
| 0.43 | `~\AppData\Roaming\Code\User` | (no area) |  | 2026-10-08 |
| 4.09 | `~\AppData\Roaming\.artomatix\Versions\ArtEngine\2022.7.2` | (no area) |  | 2022-12-29 |
| 2.42 | `~\AppData\Roaming\Adobe` | adobe/app-data | manual-review | 2025-12-08 |
| 1.15 | `~\AppData\Roaming\Adobe\UXP\PluginsStorage\PHSP` | adobe/app-data | manual-review | 2025-12-08 |
| 0.34 | `~\AppData\Roaming\Adobe\CCX Welcome` | adobe/app-data | manual-review | 2026-11-01 |
| 2.28 | `~\AppData\Roaming\npm\node_modules` | node/npm-global | manual-review | 2026-05-20 |
| 0.53 | `~\AppData\Roaming\npm\node_modules\@openai` | node/npm-global | manual-review | 2026-10-08 |
| 0.41 | `~\AppData\Roaming\npm\node_modules\@google` | node/npm-global | manual-review | 2026-04-16 |
| 0.26 | `~\AppData\Roaming\npm\node_modules\opencode-ai` | node/npm-global | manual-review | 2026-05-20 |
| 0.25 | `~\AppData\Roaming\npm\node_modules\react-native` | node/npm-global | manual-review | 2022-08-30 |
| 2.09 | `~\AppData\Roaming\nexa.ai\hyperlink\hyperlink_data` | (no area) |  | 2025-11-12 |
| 1.18 | `~\AppData\Roaming\Cursor` | (no area) |  | 2026-04-14 |
| 1.08 | `~\AppData\Roaming\Cursor\User` | (no area) |  | 2026-04-14 |
| 0.77 | `~\AppData\Roaming\Cursor\User\History` | (no area) |  | 2026-04-14 |
| 0.32 | `~\AppData\Roaming\Cursor\User\workspaceStorage` | (no area) |  | 2026-02-26 |
| 1.03 | `~\AppData\Roaming\QuadSpinner\Gaea\Autosave` | (no area) |  | 2024-01-20 |
| 0.65 | `~\AppData\Roaming\Slack` | (no area) |  | 2022-04-12 |
| 0.61 | `~\AppData\Roaming\Slack\Service Worker\CacheStorage\4c237d5e33167c88df3e45d9c8b59fdd4d727472` | (no area) |  | 2022-04-12 |
| 0.52 | `~\AppData\Roaming\Antigravity` | (no area) |  | 2026-04-09 |
| 0.39 | `~\AppData\Roaming\Bridge` | (no area) |  | 2023-09-21 |
| 0.35 | `~\AppData\Roaming\Bridge\Cache` | (no area) |  | 2023-09-21 |
| 0.38 | `~\AppData\Roaming\com.adobe.dunamis` | (no area) |  | 2026-10-20 |
| 0.36 | `~\AppData\Roaming\discord` | (no area) |  | 2026-10-31 |
| 0.33 | `~\AppData\Roaming\Elgato` | (no area) |  | 2023-01-20 |
| 0.31 | `~\AppData\Roaming\Replica Studios\Cache` | (no area) |  | 2023-09-10 |
| 0.26 | `~\AppData\Roaming\DAZ 3D` | (no area) |  | 2022-04-13 |

## ~\AppData\LocalLow

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 1.91 | `~\AppData\LocalLow` | windows/local-low | manual-review | 2026-10-10 |
| 0.36 | `~\AppData\LocalLow\Uzmi Games\ToS` | windows/local-low | manual-review | 2023-07-28 |
| 0.33 | `~\AppData\LocalLow\Uzmi Games\ToS\Icons` | windows/local-low | manual-review | 2023-05-13 |
| 0.34 | `~\AppData\LocalLow\Actoz` | windows/local-low | manual-review | 2023-11-27 |
| 0.31 | `~\AppData\LocalLow\Adobe` | windows/local-low | manual-review | 2025-04-14 |

## C:\.myclean-quarantine

| GiB | Folder | Area | Reclaim | Folder date |
|---:|---|---|---|---|
| 40.72 | `C:\.myclean-quarantine\1` | (no area) |  | ? |
| 1.44 | `C:\.myclean-quarantine\1\26568\tools` | (no area) |  | ? |
| 0.85 | `C:\.myclean-quarantine\1\26568\tools\perl\5.38.0.1` | (no area) |  | ? |
| 0.68 | `C:\.myclean-quarantine\1\26568\tools\perl\5.38.0.1\c` | (no area) |  | ? |
| 0.81 | `C:\.myclean-quarantine\1\26347\python3` | (no area) |  | ? |
| 0.36 | `C:\.myclean-quarantine\1\26347\python3\x64-windows-rel` | (no area) |  | ? |
| 0.25 | `C:\.myclean-quarantine\1\26347\python3\x64-windows-rel\PCbuild` | (no area) |  | ? |
| 0.33 | `C:\.myclean-quarantine\1\26347\python3\x64-windows-dbg` | (no area) |  | ? |
| 0.80 | `C:\.myclean-quarantine\1\26815\openssl` | (no area) |  | ? |
| 0.35 | `C:\.myclean-quarantine\1\26815\openssl\x64-windows-dbg` | (no area) |  | ? |
| 0.32 | `C:\.myclean-quarantine\1\26815\openssl\x64-windows-rel` | (no area) |  | ? |
| 0.75 | `C:\.myclean-quarantine\1\26345\openssl` | (no area) |  | ? |
| 0.35 | `C:\.myclean-quarantine\1\26345\openssl\x64-windows-dbg` | (no area) |  | ? |
| 0.32 | `C:\.myclean-quarantine\1\26345\openssl\x64-windows-rel` | (no area) |  | ? |
| 0.40 | `C:\.myclean-quarantine\1\26309\boost-test` | (no area) |  | ? |
| 0.40 | `C:\.myclean-quarantine\1\2573\chromium-1223\chrome-win64` | (no area) |  | ? |
| 0.40 | `C:\.myclean-quarantine\1\2572\chromium-1217\chrome-win64` | (no area) |  | ? |
| 0.37 | `C:\.myclean-quarantine\1\26255\boost-log` | (no area) |  | ? |
| 0.34 | `C:\.myclean-quarantine\1\2571\chromium-1187\chrome-win` | (no area) |  | ? |
| 0.29 | `C:\.myclean-quarantine\1\26330\boost-wave` | (no area) |  | ? |
| 0.28 | `C:\.myclean-quarantine\1\2578\firefox-1490\firefox` | (no area) |  | ? |
| 0.26 | `C:\.myclean-quarantine\1\2576\chromium_headless_shell-1223\chrome-headless-shell-win64` | (no area) |  | ? |
| 0.26 | `C:\.myclean-quarantine\1\2575\chromium_headless_shell-1217\chrome-headless-shell-win64` | (no area) |  | ? |
