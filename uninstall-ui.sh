#!/usr/bin/env bash
set -Eeuo pipefail
PROJECT="$HOME/vive-ultimate-dashboard"
[[ -d "$PROJECT" ]] || { echo "No dashboard directory found."; exit 0; }
echo "This removes only the dashboard application and source, NOT the VIVE driver, SDK, or SteamVR."
read -r -p "Delete $PROJECT? [y/N] " reply
[[ "$reply" == y || "$reply" == Y ]] || exit 0
rm -rf -- "$PROJECT"
echo "Dashboard removed. Driver and SDK preserved."
