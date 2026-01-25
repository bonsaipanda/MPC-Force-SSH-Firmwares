#!/bin/bash

# Step 0: Mount the disk image
# The user must provide the path to the extracted image
path_to_extracted_image="$1"

if [ -z "$path_to_extracted_image" ]; then
    echo "Error: You must provide the path to the extracted image as an argument."
    echo "Usage: $0 /path/to/extracted.img"
    exit 1
fi

# Mount the image using udisksctl (replacing gnome-disk-image-mounter)
output=$(udisksctl loop-setup -f "$path_to_extracted_image" --no-user-interaction)

# Extract the loop device path from the output
device=$(echo "$output" | grep -o '/dev/loop[0-9]*')

# Check if the loop device was created successfully
if [[ -z "$device" ]]; then
    echo "Failed to setup loop device. Exiting."
    exit 1
fi

echo "Loop device created: $device"

# Mount the loop device
mount_output=$(udisksctl mount -b "$device")

# Extract the mount point from the output
mount_point=$(echo "$mount_output" | grep -o '/media/[^ ]*')

# Check if the disk was mounted successfully
if [[ -z "$mount_point" ]]; then
    echo "Failed to mount the disk image. Exiting."
    exit 1
fi

echo "Disk image mounted at: $mount_point"

# Step 1: Find the directory that contains "etc" or "sbin" under the mount point
target_dir=$(find "$mount_point" -type d \( -name "etc" -o -name "sbin" \) -print -quit 2>/dev/null)

if [ -z "$target_dir" ]; then
    echo "No suitable directory found in $mount_point containing 'etc' or 'sbin' directories."
    exit 1
fi

# Get the base directory (UUID part of the mount point)
uuid=$(basename "$mount_point")

if [ -z "$uuid" ]; then
    echo "Could not determine UUID of the mounted image."
    exit 1
fi

echo "UUID of mounted image: $uuid"

# --- New Step: Inject LD_PRELOAD into acvs.service ---
acvs_file="$mount_point/usr/lib/systemd/system/acvs.service"
if [ -f "$acvs_file" ]; then
    echo "Found acvs.service at $acvs_file"

    # Only inject if Environment line does not already exist
    if ! grep -q '^Environment=LD_PRELOAD=/usr/lib/libforce_cursor.so' "$acvs_file"; then
        # Insert after the first [Service] line
        sudo sed -i '/^\[Service\]/a Environment=LD_PRELOAD=/usr/lib/libforce_cursor.so' "$acvs_file"
        echo "Injected LD_PRELOAD line into acvs.service"
    else
        echo "LD_PRELOAD line already present in acvs.service"
    fi
else
    echo "acvs.service not found in $acvs_file"
fi

# Step 2: Create or overwrite the symlink inside the mounted image's "etc/systemd/system/multi-user.target.wants/"
symlink_target="/usr/lib/systemd/system/sshd.service"
symlink_destination="$mount_point/etc/systemd/system/multi-user.target.wants/sshd.service"

# Unconditionally remove the existing symlink (if present) and create a new one
sudo rm -f "$symlink_destination"
echo "Removed any existing symlink at $symlink_destination."

sudo ln -s "$symlink_target" "$symlink_destination"

if [ $? -eq 0 ]; then
    echo "Symlink created successfully."
else
    echo "Failed to create symlink."
    exit 1
fi

# Step 3: Copy or overwrite modified files to their respective locations inside the disk image

# Define source and destination files with local (./) paths
declare -A files_to_copy=(
    ["./etc/shadow"]="$mount_point/etc/shadow"
    ["./etc/passwd"]="$mount_point/etc/passwd"
    ["./etc/ssh/sshd_config"]="$mount_point/etc/ssh/sshd_config"
    ["./mouse/etc/force_cursor.conf"]="$mount_point/etc/force_cursor.conf"
    ["./mouse/usr/lib/libforce_cursor.so"]="$mount_point/usr/lib/libforce_cursor.so"   
)

# Step 4: Clear SSH override folder
rm -f -r $mount_point/etc/ssh/sshd_config.d

# Unconditionally copy the files
for src in "${!files_to_copy[@]}"; do
    dest="${files_to_copy[$src]}"
    
    sudo rm -f "$dest"
    echo "Removed any existing file at $dest."
    
    sudo cp -f "$src" "$dest"
    if [ $? -eq 0 ]; then
        echo "Copied/overwritten $src to $dest"
        
        # Ensure the file has root:root ownership
        sudo chown root:root "$dest"
        if [ $? -eq 0 ]; then
            echo "Set ownership of $dest to root:root."
        else
            echo "Failed to set ownership of $dest."
            exit 1
        fi
    else
        echo "Failed to copy/overwrite $src to $dest"
        exit 1
    fi
done

# Step 5: Unmount the disk image
udisksctl unmount -b "$device"

# Step 6: Detach the loop device
udisksctl loop-delete -b "$device"

if [ $? -eq 0 ]; then
    echo "Disk image unmounted and loop device detached successfully."
else
    echo "Failed to unmount the disk image or detach the loop device."
    exit 1
fi

echo "All tasks completed successfully."
