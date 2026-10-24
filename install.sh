
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${VIVE_SDK_DIR:-$HOME/vive-ultimate-tracker-sdk}"
STEAMVR="${STEAMVR_DIR:-/opt/steamvr}"
DRIVER="$HOME/vive-ultimate"
UI="$HOME/vive-ultimate-dashboard"

log() {
  printf '\n==> %s\n' "$*"
}

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

[[ "$(uname -m)" == "aarch64" ]] ||
  die 'This release targets Steam Frame ARM64.'

HEADERS="$STEAMVR/tools/hellovr_vulkan_linux/src/openvr/headers"

[[ -f "$STEAMVR/bin/linuxarm64/libopenvr_api.so" ]] ||
  die "SteamVR ARM64 not found at $STEAMVR."

[[ -f "$HEADERS/openvr.h" ]] ||
  die 'SteamVR OpenVR headers missing.'

[[ -f "$HEADERS/openvr_driver.h" ]] ||
  die 'SteamVR driver header missing.'

SDK_REPO="https://github.com/ViveRoboticsSoftware/vive-ultimate-tracker-sdk.git"

if [[ ! -f "$SDK/include/ultimate_tracker.h" ||
      ! -f "$SDK/bin/linux/arm64/libultimate-tracker.so" ]]; then

  command -v git >/dev/null ||
    die 'Git is required to download the HTC SDK.'

  if [[ -e "$SDK" ]]; then
    die "SDK directory exists but is incomplete: $SDK"
  fi

  log 'Downloading official HTC VIVE Ultimate Tracker SDK'
  git clone --depth 1 "$SDK_REPO" "$SDK" ||
    die 'HTC SDK download failed.'
fi

SDK_LIB="$SDK/bin/linux/arm64/libultimate-tracker.so"
LIB="${VIVE_TRACKER_LIB:-$SDK_LIB}"

[[ -f "$SDK/include/ultimate_tracker.h" ]] ||
  die 'HTC SDK header missing.'

[[ -f "$LIB" ]] ||
  die "Tracker runtime not found: $LIB"

log 'Using official HTC SDK (subject to HTC license terms)'

for tool in g++ pkg-config cp mkdir ldd; do
  command -v "$tool" >/dev/null ||
    die "Missing build tool: $tool"
done

pkg-config --exists egl glesv2 gbm freetype2 ||
  die 'Missing EGL/GLESv2/GBM/FreeType development packages.'

log 'Backing up existing installation'

STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="$HOME/.local/share/vive-ultimate-manager/backups/$STAMP"

mkdir -p "$BACKUP"

for f in \
  "$DRIVER/driver.cpp" \
  "$DRIVER/driver.vrdrivermanifest" \
  "$DRIVER/lib/libultimate-tracker.so" \
  "$DRIVER/bin/linuxarm64/driver_vive_ultimate.so" \
  "$UI/dashboard.cpp" \
  "$UI/vive-ultimate-dashboard"; do

  if [[ -f "$f" ]]; then
    mkdir -p "$BACKUP/$(dirname "${f#$HOME/}")"
    cp -p "$f" "$BACKUP/${f#$HOME/}"
  fi
done

log 'Installing driver and dashboard sources'

mkdir -p "$DRIVER/bin/linuxarm64" "$DRIVER/lib" "$UI"

cp "$ROOT/driver/driver.cpp" "$DRIVER/driver.cpp"
cp "$ROOT/driver/driver.vrdrivermanifest" \
   "$DRIVER/driver.vrdrivermanifest"

cp -a "$ROOT/driver/resources" "$DRIVER/"

if [[ "$LIB" != "$DRIVER/lib/libultimate-tracker.so" ]]; then
  cp "$LIB" "$DRIVER/lib/libultimate-tracker.so"
fi

log 'Compiling SteamVR driver'

g++ -std=c++17 -O2 -fPIC -shared -Wall -Wextra \
  -I"$SDK/include" \
  -I"$HEADERS" \
  "$DRIVER/driver.cpp" \
  -L"$DRIVER/lib" \
  -Wl,-rpath,'$ORIGIN/../../lib' \
  -lultimate-tracker \
  -pthread \
  -o "$DRIVER/bin/linuxarm64/driver_vive_ultimate.so"

log 'Checking driver dependencies'

if ldd "$DRIVER/bin/linuxarm64/driver_vive_ultimate.so" | grep 'not found'; then
  die 'Driver has unresolved shared-library dependencies.'
fi

log 'Compiling dashboard'

cp "$ROOT/dashboard/dashboard.cpp" "$UI/dashboard.cpp"
cp "$ROOT/dashboard/build.sh" "$UI/build.sh"

chmod +x "$UI/build.sh"
"$UI/build.sh"

log 'Registering SteamVR driver'

VRPATHREG="$STEAMVR/bin/linuxarm64/vrpathreg"

if [[ -x "$VRPATHREG" ]]; then
  "$VRPATHREG" adddriver "$DRIVER" ||
    echo 'WARNING: Automatic driver registration failed.'
else
  echo 'WARNING: vrpathreg not found.'
fi

log 'Installation completed'
echo 'Restart SteamVR and test tracker detection.'
echo 'Automatic dashboard startup is not yet configured.'
echo "Backup: $BACKUP"

