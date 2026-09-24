#!/usr/bin/env bash
# Builds CoffeeFlix and copies it to a Wii U running an FTP server (such as ftpiiu).
#   WIIU_IP=192.168.1.20 tools/deploy.sh
set -euo pipefail
cd "$(dirname "$0")/.."
: "${WIIU_IP:?Set WIIU_IP to your Wii U's IP address}"

make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
lftp -u anonymous,anonymous -e "set ftp:passive-mode off; cd /fs/vol/external01/wiiu/apps/coffeeflix; put coffeeflix.wuhb; bye" "$WIIU_IP"
echo "Copied coffeeflix.wuhb to $WIIU_IP. Restart the Homebrew Launcher to load it."
