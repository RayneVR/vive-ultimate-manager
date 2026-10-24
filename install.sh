#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${VIVE_SDK_DIR:-$HOME/vive-ultimate-tracker-sdk}"
STEAMVR="${STEAMVR_DIR:-/opt/steamvr}"
DRIVER="$HOME/vive-ultimate"
UI="$HOME/vive-ultimate-dashboard"
log(){ printf '\n==> %s\n' "$*"; }
die(){ printf 'ERROR: %s\n' "$*" >&2; exit 1; }
[[ "$(uname -m)" == aarch64 ]] || die 'This release targets Steam Frame ARM64.'
[[ -f "$STEAMVR/bin/linuxarm64/libopenvr_api.so" ]] || die "SteamVR ARM64 not found at $STEAMVR. Set STEAMVR_DIR."
[[ -f "$STEAMVR/tools/hellovr_vulkan_linux/src/openvr/headers/openvr.h" ]] || die 'SteamVR OpenVR headers missing.'
[[ -f "$STEAMVR/tools/hellovr_vulkan_linux/src/openvr/headers/openvr_driver.h" ]] || die 'SteamVR driver header missing.'
[[ -f "$SDK/include/ultimate_tracker.h" ]] || die "Authorized VIVE SDK headers required at $SDK/include. Set VIVE_SDK_DIR."
[[ -f "$SDK/LICENSE" || -f "$SDK/README.md" ]] || echo 'Note: verify your SDK licensing before distributing.'
LIB="${VIVE_TRACKER_LIB:-$DRIVER/lib/libultimate-tracker.so}"
[[ -f "$LIB" ]] || die 'Proprietary libultimate-tracker.so is missing. Supply an authorized copy via VIVE_TRACKER_LIB=/path/to/libultimate-tracker.so.'
for tool in g++ pkg-config cp mkdir; do command -v "$tool" >/dev/null || die "Missing build tool: $tool"; done
pkg-config --exists egl glesv2 gbm freetype2 || die 'Missing EGL/GLESv2/GBM/FreeType development packages. Install with your SteamOS package manager.'
log 'Backing up existing installations'
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="$HOME/.local/share/vive-ultimate-manager/backups/$STAMP"
mkdir -p "$BACKUP"
for f in "$DRIVER/driver.cpp" "$DRIVER/driver.vrdrivermanifest" "$DRIVER/bin/linuxarm64/driver_vive_ultimate.so" "$UI/dashboard.cpp" "$UI/vive-ultimate-dashboard"; do
  if [[ -f "$f" ]]; then mkdir -p "$BACKUP/$(dirname "${f#$HOME/}")"; cp -p "$f" "$BACKUP/${f#$HOME/}"; fi
done
log 'Installing driver and dashboard sources'
mkdir -p "$DRIVER/bin/linuxarm64" "$DRIVER/lib" "$UI"
cp "$ROOT/driver/driver.cpp" "$DRIVER/driver.cpp"
cp "$ROOT/driver/driver.vrdrivermanifest" "$DRIVER/driver.vrdrivermanifest"
cp -a "$ROOT/driver/resources" "$DRIVER/"
if [[ "$LIB" != "$DRIVER/lib/libultimate-tracker.so" ]]; then cp "$LIB" "$DRIVER/lib/libultimate-tracker.so"; fi
log 'Compiling SteamVR driver'
g++ -std=c++17 -O2 -fPIC -shared -Wall -Wextra \
  -I"$SDK/include" -I"$STEAMVR/tools/hellovr_vulkan_linux/src/openvr/headers" \
  "$DRIVER/driver.cpp" -L"$DRIVER/lib" -Wl,-rpath,"$DRIVER/lib" \
  -lultimate-tracker -pthread -o "$DRIVER/bin/linuxarm64/driver_vive_ultimate.so"
log 'Compiling dashboard'
cp "$ROOT/dashboard/dashboard.cpp" "$UI/dashboard.cpp"
cp "$ROOT/dashboard/build.sh" "$UI/build.sh"
chmod +x "$UI/build.sh"
"$UI/build.sh"
log 'Registering SteamVR driver'
VRPATHREG="$STEAMVR/bin/linuxarm64/vrpathreg"
if [[ -x "$VRPATHREG" ]]; then
  "$VRPATHREG" adddriver "$DRIVER" || echo 'WARNING: automatic driver registration failed; register manually in SteamVR.'
else
  echo 'WARNING: vrpathreg not found. SteamVR driver must be registered manually.'
fi
log 'Finished; restart SteamVR and test tracker detection. No automatic launch integration is configured.'
echo "Backup: $BACKUP"
