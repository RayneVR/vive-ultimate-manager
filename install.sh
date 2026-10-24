#!/usr/bin/env bash
set -Eeuo pipefail
PROJECT="${HOME}/vive-ultimate-dashboard"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
fail(){ echo "ERROR: $*" >&2; exit 1; }
[[ "$(uname -m)" == aarch64 ]] || echo "Warning: designed for Steam Frame ARM64 Linux."
[[ -f "$PROJECT/build.sh" ]] || fail "Existing working dashboard/build.sh required at $PROJECT. See README.md."
[[ -f "$HOME/vive-ultimate-tracker-sdk/include/ultimate_tracker.h" ]] || fail "VIVE SDK missing at ~/vive-ultimate-tracker-sdk."
[[ -f "$HOME/vive-ultimate/lib/libultimate-tracker.so" ]] || fail "Tracker driver library missing at ~/vive-ultimate/lib/."
[[ -f /opt/steamvr/bin/linuxarm64/libopenvr_api.so ]] || fail "SteamVR ARM64 runtime missing at /opt/steamvr."
command -v g++ >/dev/null || fail "g++ not installed."
[[ -f "$HERE/dashboard.cpp" ]] || fail "dashboard.cpp missing from package."
mkdir -p "$PROJECT/backups"
stamp="$(date +%Y%m%d-%H%M%S)"
[[ ! -f "$PROJECT/dashboard.cpp" ]] || cp "$PROJECT/dashboard.cpp" "$PROJECT/backups/dashboard.$stamp.cpp"
[[ ! -f "$PROJECT/vive-ultimate-dashboard" ]] || cp "$PROJECT/vive-ultimate-dashboard" "$PROJECT/backups/vive-ultimate-dashboard.$stamp"
cp "$HERE/dashboard.cpp" "$PROJECT/dashboard.cpp"
if ! (cd "$PROJECT" && bash ./build.sh); then
  if [[ -f "$PROJECT/backups/dashboard.$stamp.cpp" ]]; then cp "$PROJECT/backups/dashboard.$stamp.cpp" "$PROJECT/dashboard.cpp"; fi
  if [[ -f "$PROJECT/backups/vive-ultimate-dashboard.$stamp" ]]; then cp "$PROJECT/backups/vive-ultimate-dashboard.$stamp" "$PROJECT/vive-ultimate-dashboard"; fi
  fail "Build failed. Previous source and binary restored."
fi
cp "$PROJECT/dashboard.cpp" "$PROJECT/dashboard.cpp.CURRENT"
cp "$PROJECT/vive-ultimate-dashboard" "$PROJECT/vive-ultimate-dashboard.CURRENT"
echo "Installed successfully: $PROJECT/vive-ultimate-dashboard"
echo "Launch with SteamVR active: $PROJECT/vive-ultimate-dashboard"
echo "Previous version saved in $PROJECT/backups/"
