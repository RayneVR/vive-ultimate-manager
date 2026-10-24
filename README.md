# VIVE Ultimate Manager — Steam Frame

Community SteamVR dashboard UI for VIVE Ultimate Trackers on Steam Frame (ARM64 Linux).

## Features
- Live tracker connection and pose status (full pose, rotation only, tracking lost)
- Tracker dropdown with Echo / power-off commands
- OSC configuration
- Pair/unpair and service warnings
- Room setup placeholder (shows “In progress” on hover)

## Install / update on an already-working Steam Frame

**Prerequisites:** This package currently requires a working tracker driver, VIVE SDK, SteamVR runtime, and `~/vive-ultimate-dashboard/build.sh`. It **does not** install the underlying driver, SDK, SteamVR, or system packages. It is not yet a fresh-device one-click installer.

After publishing this repository on GitHub:

```bash
git clone https://github.com/YOUR-USERNAME/vive-ultimate-manager.git
cd vive-ultimate-manager
bash install.sh
```

Or download and extract the GitHub repository ZIP and run `bash install.sh` from the extracted folder.

### What the installer does
1. Checks required files and tools.
2. Backs up the current source and binary to `~/vive-ultimate-dashboard/backups/`.
3. Copies the dashboard source into the existing project.
4. Runs the **existing known-working** `build.sh`.
5. Restores the previous source/binary if compilation fails.
6. Saves `.CURRENT` copies after a successful build.

Launch with SteamVR active:

```bash
~/vive-ultimate-dashboard/vive-ultimate-dashboard
```

## Uninstall UI

```bash
bash uninstall-ui.sh
```

This removes only the dashboard project; it does not remove SteamVR, VIVE driver, or SDK. **Back up your personal changes first.**

## Before a public release
- Verify ownership and redistribution rights for dashboard source and dependencies.
- Obtain permission or a documented distribution route for any proprietary SDK or driver assets.
- Add a fresh-install driver/SDK workflow, service registration, and SteamVR auto-start integration after testing on a clean Frame.
- Test installer on Steam Frame ARM64; this package has not been compiled or run on a Frame by the packager.

## Known limitations
- The Echo command is not guaranteed to physically identify a tracker.
- Room setup is not yet implemented.
- The tracking-lost timeout is heuristic.
- The dashboard includes an existing absolute icon path (`/home/steamos/vive-ultimate/resources/icons/tracker_status_off@2x.png`), so icons may be missing on other installations.
