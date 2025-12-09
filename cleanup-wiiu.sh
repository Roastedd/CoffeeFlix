#!/bin/bash
# Clean up old CafeMP builds on Wii U

WIIU_IP="10.0.0.123"
WIIU_PATH="/fs/vol/external01/wiiu/apps/CafeMP"

echo "Cleaning up old builds on Wii U..."

lftp -e "set ftp:passive-mode off; cd ${WIIU_PATH}; \
    mrm CafeMP*.wuhb.new; \
    mrm CafeMP*_temp.wuhb; \
    mrm CafeMP*_new.wuhb; \
    mrm CafeMP*.rpx; \
    ls; bye" -u anonymous,anonymous ${WIIU_IP}

echo "✓ Cleanup complete!"
