## Overview
This patch enables the hardware cursor in MPC by intercepting DRM (Direct Rendering Manager) calls that hide the cursor. It uses LD_PRELOAD to hook into the MPC binary without modifying it.

## How It Works
- **Intercepts `drmModeSetCursor2()`** - Blocks cursor hide attempts and shows custom cursor
- **Custom circular cursor** - Displays a white circular cursor on screen
- **Mouse movement tracking** - Monitors mouse input and moves cursor in real-time
- **Click support** - Converts mouse clicks to touch events for MPC interaction
- **Uses MockbaMod's LD_PRELOAD** - Integrates with existing boot system


# Manual Installation Guide - Force Hardware Cursor Patch

Simple guide to compile and install the cursor patch manually by editing files on the SD card.

---

## Step 1: Compile the Patch Using Docker. if not already included!

Make sure you have Docker installed on your computer, then run this command:

```bash
docker run --rm -v /tmp:/tmp debian:bullseye bash -c "
dpkg --add-architecture armhf &&
apt update -qq &&
apt install -y -qq gcc-arm-linux-gnueabihf libdrm-dev:armhf &&
arm-linux-gnueabihf-gcc -shared -fPIC \
  -I/usr/include/libdrm \
  -I/usr/include/arm-linux-gnueabihf \
  -o libforce_cursor.so \
  force_cursor.c \
  -ldl -lpthread \
  -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard &&
echo '✓ Compiled successfully'
"
```

**What this does:**
- Runs a Debian container with ARM cross-compiler
- Compiles `force_cursor.c` → `libforce_cursor.so`
- Creates an ARM binary that works on your Force

**Result:** You'll have `libforce_cursor.so` ready to copy.

---

## Step 2: Copy Files to SD Card

### Option A: Using SSH (easiest)

```bash
scp libforce_cursor.so root@192.168.2.31:/media/662522/MockbaMod/
```

### Option B: Manually via SD Card

1. **Eject the SD card** from your Force
2. **Insert into your computer**
3. **Copy the file:**
   - Copy `libforce_cursor.so` from your computer
   - Paste to SD card at: `/MockbaMod/libforce_cursor.so`
4. **Eject safely** and put SD card back in Force

---

## Step 3: Enable the Cursor Patch

### Option A: Using SSH

```bash
ssh root@192.168.2.31
echo "/media/662522/MockbaMod/libforce_cursor.so" > /dev/shm/.LD_PRELOAD
systemctl restart acvs.service
```

### Option B: Create Enable Script on SD Card

1. **Create a file** on the SD card at: `/enablecursor.sh`
2. **Paste this content:**

```bash
#!/bin/sh
# Enable hardware cursor in MPC

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

LD_LIB="/media/662522/MockbaMod/libforce_cursor.so"

if [ -f "$mmLD_PRELOAD_VAR" ]; then
    FC=$(cat "$mmLD_PRELOAD_VAR")
    if [[ "$FC" != *"$LD_LIB"* ]]; then
        # Prepend to beginning (load before other libs)
        echo "$LD_LIB $FC" > "$mmLD_PRELOAD_VAR"
        echo "Hardware cursor enabled!"
    else
        echo "Hardware cursor already enabled!"
    fi
else
    echo "$LD_LIB" > "$mmLD_PRELOAD_VAR"
    echo "Hardware cursor enabled!"
fi

echo "Restart MPC to apply: systemctl restart acvs.service"
```

3. **Make it executable** (via SSH):

```bash
ssh root@192.168.2.31 "chmod +x /media/662522/enablecursor.sh"
```

4. **Run it:**

```bash
ssh root@192.168.2.31 "/media/662522/enablecursor.sh"
ssh root@192.168.2.31 "systemctl restart acvs.service"
```

---

## Step 4: Disable the Cursor Patch

### Option A: Quick Disable (SSH) **Warning:** **Warning:**

```bash
ssh root@192.168.2.31
rm -f /dev/shm/.LD_PRELOAD
systemctl restart acvs.service
```

**Warning:** This removes ALL LD_PRELOAD libraries, including MockbaMagic and MidiLoop!

### Option B: Proper Disable (Keeps Other Mods)

1. **Create a file** on SD card at: `/disablecursor.sh`
2. **Paste this content:**

```bash
#!/bin/sh
# Disable hardware cursor patch

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

LD_LIB="/media/662522/MockbaMod/libforce_cursor.so"

if [ -f "$mmLD_PRELOAD_VAR" ]; then
    FC=$(cat "$mmLD_PRELOAD_VAR")
    # Remove our library from the list
    NEW_FC=$(echo "$FC" | sed "s|$LD_LIB||g" | sed "s/  / /g" | sed "s/^ //g" | sed "s/ $//g")
    if [ -n "$NEW_FC" ]; then
        echo "$NEW_FC" > "$mmLD_PRELOAD_VAR"
    else
        rm -f "$mmLD_PRELOAD_VAR"
    fi
    echo "Hardware cursor disabled!"
else
    echo "Hardware cursor was not enabled"
fi

echo "Restart MPC to apply: systemctl restart acvs.service"
```

3. **Make it executable:**

```bash
ssh root@192.168.2.31 "chmod +x /media/662522/disablecursor.sh"
```

4. **Run it:**

```bash
ssh root@192.168.2.31 "/media/662522/disablecursor.sh"
ssh root@192.168.2.31 "systemctl restart acvs.service"
```

---

## Verification

### Check if Patch is Loaded

```bash
ssh root@192.168.2.31 "cat /dev/shm/.LD_PRELOAD"
```

**Should show:**
```
/media/662522/MockbaMod/libforce_cursor.so /media/662522/AddOns/mockbaMagic/mockbaMagic.so ...
```

### Check if MPC Loaded It

```bash
ssh root@192.168.2.31 "pgrep MPC | while read pid; do grep -q libforce_cursor /proc/\$pid/maps && echo 'Patch loaded in PID '\$pid; done"
```

**Should show:**
```
Patch loaded in PID 1234
```

---

## Uninstall Completely

1. **Disable the patch** (see Step 4)
2. **Remove files from SD card:**
   ```bash
   ssh root@192.168.2.31
   rm /media/662522/MockbaMod/libforce_cursor.so
   rm /media/662522/enablecursor.sh
   rm /media/662522/disablecursor.sh
   systemctl restart acvs.service
   ```

Or manually delete these files from the SD card:
- `/MockbaMod/libforce_cursor.so`
- `/enablecursor.sh`
- `/disablecursor.sh`

---

## Troubleshooting

### Cursor Not Visible

1. Check if library exists:
   ```bash
   ssh root@192.168.2.31 "ls -lh /media/662522/MockbaMod/libforce_cursor.so"
   ```

2. Check LD_PRELOAD:
   ```bash
   ssh root@192.168.2.31 "cat /dev/shm/.LD_PRELOAD"
   ```

3. Check if loaded in MPC:
   ```bash
   ssh root@192.168.2.31 "pgrep MPC | while read pid; do grep libforce_cursor /proc/\$pid/maps; done"
   ```

### MPC Crashes After Enabling

Disable the patch immediately:
```bash
ssh root@192.168.2.31 "/media/662522/disablecursor.sh"
ssh root@192.168.2.31 "systemctl restart acvs.service"
```

### Mouse Not Detected

Check mouse device:
```bash
ssh root@192.168.2.31 "cat /proc/bus/input/devices | grep -A5 Mouse"
```

If your mouse is on a different event (not event2), edit `force_cursor.c` line ~153:
```c
int fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
```
Change `event2` to your mouse event number, then recompile.

---

## File Locations Summary

| File | Location | Purpose |
|------|----------|---------|
| Source code | `/tmp/force_cursor.c` | C source to compile |
| Compiled library | `/tmp/libforce_cursor.so` → `/media/662522/MockbaMod/libforce_cursor.so` | ARM binary for Force |
| Enable script | `/media/662522/enablecursor.sh` | Turns on cursor |
| Disable script | `/media/662522/disablecursor.sh` | Turns off cursor |
| LD_PRELOAD config | `/dev/shm/.LD_PRELOAD` | Runtime config (tmpfs) |
| Env config | `/media/662522/MockbaMod/env.sh` | Defines variables |

---

## Quick Reference

**Enable cursor:**
```bash
ssh root@192.168.2.31 "/media/662522/enablecursor.sh && systemctl restart acvs.service"
```

**Disable cursor:**
```bash
ssh root@192.168.2.31 "/media/662522/disablecursor.sh && systemctl restart acvs.service"
```

**Check status:**
```bash
ssh root@192.168.2.31 "cat /dev/shm/.LD_PRELOAD"
```

---

## Notes

- The cursor is a **white circle** that appears at the **bottom-left** when MPC starts
- Mouse movement is **swapped/inverted** to match the Force's portrait screen orientation
- **Left-click** works as touch/tap
- The patch integrates with MockbaMod's existing LD_PRELOAD system
- **Does not modify** the MPC binary - fully reversible
- Works with **Microsoft Trackball Explorer** on `/dev/input/event2`

Enjoy your hardware cursor! 🎵🖱️
