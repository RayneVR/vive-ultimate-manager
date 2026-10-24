#!/usr/bin/env bash
set -euo pipefail
SDK="$HOME/vive-ultimate-tracker-sdk"
DRIVER="$HOME/vive-ultimate"
STEAMVR="/opt/steamvr"
OUT="$HOME/vive-ultimate-dashboard"
mkdir -p "$OUT"
test -f "$SDK/include/ultimate_tracker.h"
test -f "$DRIVER/lib/libultimate-tracker.so"
test -f "$STEAMVR/bin/linuxarm64/libopenvr_api.so"
g++ -std=c++17 -O2 -Wall -Wextra \
  -I"$SDK/include" \
  -I"$STEAMVR/tools/hellovr_vulkan_linux/src/openvr/headers" \
  "$OUT/dashboard.cpp" \
  -L"$DRIVER/lib" -Wl,-rpath,"$DRIVER/lib" \
  -L"$STEAMVR/bin/linuxarm64" -Wl,-rpath,"$STEAMVR/bin/linuxarm64" \
  -lultimate-tracker -lopenvr_api -pthread $(pkg-config --cflags --libs egl glesv2 gbm freetype2) \
  -o "$OUT/vive-ultimate-dashboard"
echo "Built: $OUT/vive-ultimate-dashboard"
echo "Run with SteamVR active: $OUT/vive-ultimate-dashboard"
