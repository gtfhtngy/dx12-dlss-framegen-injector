# DX12 DLSS Frame Generation Injector

A DLL-based injector that enables **NVIDIA DLSS Frame Generation** (via Streamline) in compatible games that do not natively support it.

## Current Status

- Tested on **Scarlet Nexus** and **Code Vein** and **Captain Tsubasa: World Fighters**
- All three games use Unreal Engine + DirectX 12
- Frame Generation works
- Some stuttering can occur in very busy scenes (expected behavior with frame generation)
- No major ghosting observed during normal gameplay in the tested scenarios
- Full source code available

**Important:** Compatibility with other games is currently a **hypothesis** and has not been confirmed beyond the two tested titles.

## How it works (high level)

The injector acts as a `winmm.dll` proxy.  
It initializes NVIDIA Streamline early, hooks the necessary DirectX 12 / DXGI calls, and supplies the required camera matrices, depth, and motion vectors so that DLSS Frame Generation can run.

It includes automatic detection of different UE4 view-buffer layouts to improve compatibility across engine versions.

## Tested Games

| Game            | Engine | API   | Result     | Notes                                      |
|-----------------|--------|-------|------------|--------------------------------------------|
| Scarlet Nexus   | UE4    | DX12  | Working    | Primary test case                          |
| Code Vein       | UE4    | DX12  | Working    | Older UE4 view-buffer layout supported (DX12 Makes the game crashes after the end cutscene)    |
| Captain Tsubasa: World Fighters      | UE5    | DX12  | Working    | UE5 Test (Flickering screen loading and menu) (**it only works good on TSR pls do NOT run it on any other ANTI-ALIASING**)    |

## Installation

### 1. Files you need

| File | Where to get it | Notes |
| ---- | --------------- | ----- |
| `winmm.dll` | This project (Releases page, or build it yourself) | The injector itself |
| `sl.interposer.dll` | NVIDIA Streamline SDK | Required |
| `sl.common.dll` | NVIDIA Streamline SDK | Required |
| `sl.dlss_g.dll` | NVIDIA Streamline SDK | Frame Generation plugin |
| `sl.reflex.dll` | NVIDIA Streamline SDK | Required by DLSS-G |
| `sl.pcl.dll` | NVIDIA Streamline SDK | Required by DLSS-G |
| `nvngx_dlssg.dll` | NVIDIA Streamline SDK | DLSS-G model |
| config file | `config/` folder in this repo | Copy and edit if needed |

Get the Streamline files from NVIDIA's official repository
(https://github.com/NVIDIAGameWorks/Streamline). Use the **production**
(not development) DLLs from the SDK's `bin/x64` folder. Use the same SDK
version for all files. The versions I tested with: **2.14.1**.

These files are NOT included in this repository or in the releases.

### 2. Folder layout

Put everything in the same folder as the game's executable
(the `.exe` inside `...\Binaries\Win64\` for Unreal Engine games):

    Binaries\Win64\
    ├── ScarletNexus-Win64-Shipping.exe     (the game's own exe)
    ├── winmm.dll
    ├── sl.interposer.dll
    ├── sl.common.dll
    ├── sl.dlss_g.dll
    ├── sl.reflex.dll
    ├── sl.pcl.dll
    ├── nvngx_dlssg.dll
    └── SN_DLSSG_cfg.txt

### 3. Run

1. Launch the game in **DirectX 12 mode**.
2. To uninstall, delete the files you added.

### Troubleshooting

- Game crashes at startup: check that all Streamline DLLs come from the
  same SDK version, and that the game is running in DX12.
- Please attach the log when opening an issue.

> Detailed usage instructions, Streamline file requirements, and config options will be expanded as more testing is completed.

## Building

```
# On Linux (requires git + mingw-w64)
sudo apt install g++-mingw-w64-x86-64-posix git
cd src
sh build.sh
```

This produces `winmm.dll`. Dependencies (MinHook + Streamline headers)
are fetched automatically at build time.

Dependencies (MinHook + Streamline headers) are fetched automatically at build time.
Known Limitations

Currently only confirmed on two UE4 DX12 titles.
Stuttering can appear in very dense scenes (common with frame generation).
Broader compatibility is untested.
Requires official NVIDIA Streamline / DLSS-G runtime files (these are not redistributed with this project).

Technical Notes

No NVIDIA proprietary binaries are redistributed.
Streamline headers and MinHook are fetched at build time.
See THIRD_PARTY_NOTICES.md for details.

## Development

This project was written entirely by an AI model (Claude Sonnet 5.5, by
Anthropic). I directed the work, tested it on real games, and verified
the results myself. I did not write the code by hand.

Disclaimer
This project is not affiliated with, endorsed by, or connected to NVIDIA Corporation.

NVIDIA, DLSS, Streamline, and RTX are trademarks of NVIDIA Corporation.

Use at your own risk. Always back up your game files.
