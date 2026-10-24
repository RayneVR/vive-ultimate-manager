# Dashboard — VIVE Ultimate Manager for Steam Frame

A SteamVR dashboard and tracker driver for VIVE Ultimate Trackers on Steam Frame ARM64.

**Release UI:** tracker dropdown controls, OSC settings, pose-status indicators, two-line pairing/dongle warning, greyed-out Room setup ("In progress" on hover), and a bottom-right **RayneVR - GitHub** link to https://github.com/RayneVR.

## Installation (prerelease)

On a Steam Frame with SteamVR and required development packages available:

```bash
git clone https://github.com/RayneVR/vive-ultimate-manager.git
cd vive-ultimate-manager
bash install.sh
```

The installer downloads the HTC SDK from https://github.com/ViveRoboticsSoftware/vive-ultimate-tracker-sdk if absent, compiles the driver and dashboard, and attempts to register the driver with SteamVR. It backs up existing source and binaries before replacement. Restart SteamVR afterward.

**Not yet a tested, fully unattended fresh install.** SteamVR must be installed at `/opt/steamvr` (or set `STEAMVR_DIR`); ARM64 OpenVR headers, compiler and EGL/GLESv2/GBM/FreeType development packages must already be available. The dashboard is **not automatically launched**. The installer will stop with a diagnostic if prerequisites are missing.

Override SDK location with `VIVE_SDK_DIR=/path/to/sdk bash install.sh` if needed.

## Distribution and licensing

The SDK is fetched from HTC's public repository rather than included here. Its use is subject to HTC's terms. Review rights and licenses for the driver code, SteamVR resources and third-party assets before making the repository public.

## Project layout

- `install.sh`: source installation, compilation, SteamVR registration attempt
- `dashboard/dashboard.cpp`: dashboard UI
- `dashboard/build.sh`: UI build instructions
- `driver/driver.cpp`: SteamVR driver source
- `driver/driver.vrdrivermanifest`: driver manifest
- `driver/resources/`: SteamVR resource files
