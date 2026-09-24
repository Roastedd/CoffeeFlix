#!/bin/bash
# CoffeeFlix Quick Deploy Script
# Builds and deploys to Wii U without full reboot

set -e

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

WIIU_IP="${WIIU_IP:-10.0.0.123}"
WIIU_PATH="/fs/vol/external01/wiiu/apps/coffeeflix"

echo -e "${BLUE}=== CoffeeFlix Quick Deploy ===${NC}"

# Set devkitPro environment
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=/opt/devkitpro/devkitPPC
export WUT_ROOT=/opt/devkitpro/wut

# Build
echo -e "${BLUE}Building CoffeeFlix...${NC}"
make -j$(sysctl -n hw.ncpu)

if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

# Upload
echo -e "${BLUE}Uploading to Wii U at ${WIIU_IP}...${NC}"
lftp -e "set ftp:passive-mode off; cd ${WIIU_PATH}; put coffeeflix.wuhb; bye" -u anonymous,anonymous ${WIIU_IP}

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Deploy complete!${NC}"
    echo -e "${GREEN}Now use WiiUReboot to restart Homebrew Launcher${NC}"
else
    echo -e "${RED}Upload failed!${NC}"
    exit 1
fi
