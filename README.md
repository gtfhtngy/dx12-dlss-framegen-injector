# DX12 DLSS Frame Generation Injector

A DLL-based injector that enables **NVIDIA DLSS Frame Generation** (via Streamline) in compatible games.

This project is **completely separate** from the DLAA / DLSS Super Resolution injector.

## Current Status

- Tested on **Scarlet Nexus** and **Code Vein**
- Both games are Unreal Engine 4, DirectX 12
- Frame Generation works; some stuttering can occur in very busy scenes (expected behavior when generating frames)
- No major ghosting observed during normal gameplay in the tested scenarios
- Source code is fully available

**Important:** Compatibility with other games is currently a **hypothesis** and has not been confirmed beyond the two tested titles.

## How it works (high level)

The injector acts as a `winmm.dll` proxy.  
It initializes NVIDIA Streamline early, hooks the necessary DirectX 12 / DXGI calls, and feeds the required camera matrices, depth and motion vectors so that DLSS Frame Generation can run.

It includes automatic detection of UE4 view-buffer layouts for better compatibility across different engine versions.

## Tested Games

| Game            | Engine | API   | Result                          | Notes                                      |
|-----------------|--------|-------|---------------------------------|--------------------------------------------|
| Scarlet Nexus   | UE4    | DX12  | Working                         | Primary test case                          |
| Code Vein       | UE4    | DX12  | Working                         | Older UE4 view-buffer layout supported     |

## Installation (basic)

1. Build the project (see below) or use a release binary when available.
2. Place `winmm.dll` next to the game executable.
3. Place the required Streamline / DLSS-G files (obtained from official NVIDIA sources) next to the executable or in a folder specified by config.
4. Place a config file (example provided in `config/`) next to the executable.
5. Launch the game.

> Detailed usage, Streamline file requirements, and config options will be expanded with further testing.

## Building

```bash
# On Linux (requires git + mingw-w64)
sudo apt install g++-mingw-w64-x86-64-posix git
cd src
sh build.sh
This produces winmm.dll.

Dependencies (MinHook + Streamline headers) are fetched automatically at build time.
Known Limitations

Currently only confirmed on two UE4 DX12 titles.
Stuttering can appear in very dense scenes (common with frame generation).
Broader compatibility is untested.
Requires official NVIDIA Streamline / DLSS-G runtime files (not redistributed).

Technical Notes

No NVIDIA proprietary binaries are redistributed.
Streamline headers and MinHook are fetched at build time.
See THIRD_PARTY_NOTICES.md for details.

Development
This project was developed with assistance from Claude Sonnet 5.5.
Disclaimer
This project is not affiliated with, endorsed by, or connected to NVIDIA Corporation.

NVIDIA, DLSS, Streamline, and RTX are trademarks of NVIDIA Corporation.

Use at your own risk. Always back up your game files.
