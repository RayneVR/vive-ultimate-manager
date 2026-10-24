# VIVE Ultimate Manager — Steam Frame ARM64 (pre-release)

SteamVR driver and dashboard source.

**Room Setup not functional yet, Must use a PC to set up a Tracking Map!!!**.

## Installation

This repository includes the original dashboard and custom driver sources and SteamVR resources. **It does not redistribute HTC proprietary SDK or `libultimate-tracker.so`.** You must obtain these from an authorized source and have them on the Frame.

Requirements: Steam Frame ARM64, SteamVR installed at `/opt/steamvr`, VIVE SDK headers at `~/vive-ultimate-tracker-sdk/include/ultimate_tracker.h`, authorized `libultimate-tracker.so` at `~/vive-ultimate/lib/`, and C++/pkg-config/EGL/GLESv2/GBM/FreeType development dependencies.

```bash
git clone https://github.com/RayneVR/vive-ultimate-manager.git
cd vive-ultimate-manager
bash install.sh
```

For a different authorized SDK/library location:

```bash
VIVE_SDK_DIR=/path/to/sdk VIVE_TRACKER_LIB=/path/to/libultimate-tracker.so bash install.sh
```

The installer checks prerequisites, backs up previous source/binaries, compiles the driver and UI, and attempts driver registration using `vrpathreg`. Restart SteamVR afterward. This is **not yet a verified fresh-install solution**: the proprietary runtime must be supplied, and automatic dashboard startup is not yet configured. Test on a spare device before publishing as a stable release.

## License and redistribution

The supplied source and resources originate from the user's Frame backup. Verify authorship and licenses of the driver, resources, and any third-party code before publishing. Do not publish proprietary HTC SDK binaries without redistribution permission.
