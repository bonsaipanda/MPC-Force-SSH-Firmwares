#!/bin/bash

# Exit on error
set -e

# Resolve script directory
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Check input
if [ -z "$1" ]; then
    echo "Usage: sh patch_firmware.sh <input_firmware.img>"
    exit 1
fi

# Absolute path to input
INPUT_IMG="$(realpath "$1")"

# Extract base name
BASENAME=$(basename "$INPUT_IMG" -update.img)

# Output paths (outside ssh/)
WORK_IMG="$SCRIPT_DIR/mpc.img"
OUTPUT_IMG="$SCRIPT_DIR/${BASENAME}-mouse-ssh-update.img"

echo ">> Entering ssh directory..."
cd "$SCRIPT_DIR/ssh"

echo ">> Extracting firmware..."
./mpcimg2 -r "$INPUT_IMG" "$WORK_IMG"

echo ">> Patching firmware (SSH)..."
sudo bash ./ssh_image.sh "$WORK_IMG"

echo ">> Repacking firmware..."
./mpcimg2 -m "$INPUT_IMG" "$WORK_IMG" "$OUTPUT_IMG"

echo ">> Leaving ssh directory..."
cd "$SCRIPT_DIR"

# Cleanup
if [ -f "$WORK_IMG" ]; then
    echo ">> Cleaning up temporary file..."
    rm -f "$WORK_IMG"
fi

echo ">> Done!"
echo "Output: $OUTPUT_IMG"
