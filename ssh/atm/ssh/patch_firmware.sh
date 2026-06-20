#!/bin/bash

# Exit on error
set -e

# Check if argument is provided
if [ -z "$1" ]; then
    echo "Usage: sh patch_firmware.sh <input_firmware.img>"
    exit 1
fi

INPUT_IMG="$1"

# Extract base name (remove -update.img)
BASENAME=$(basename "$INPUT_IMG" -update.img)

# Go into ssh directory
echo ">> Entering ssh directory..."
cd ssh

# Use paths relative to ssh/
INPUT_IMG="../$INPUT_IMG"
WORK_IMG="../mpc.img"
OUTPUT_IMG="../${BASENAME}-mouse-ssh-update.img"

echo ">> Extracting firmware..."
./mpcimg2 -r "$INPUT_IMG" "$WORK_IMG"

echo ">> Patching firmware (SSH)..."
sudo bash ssh_image.sh "$WORK_IMG"

echo ">> Repacking firmware..."
./mpcimg2 -m "$INPUT_IMG" "$WORK_IMG" "$OUTPUT_IMG"

# Return back
echo ">> Leaving ssh directory..."
cd ..

echo ">> Done!"
echo "Output: $OUTPUT_IMG"
