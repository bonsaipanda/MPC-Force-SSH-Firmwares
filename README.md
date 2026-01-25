# MPC-Force-SSH-Firmwares
Modified AKAI MPC and FORCE firmwares with SSH and mouse control enabled.

Credit for the SSH tools goes to [TheKikGen](https://github.com/TheKikGen/MPC-LiveXplore), mouse addon credit goes to no3z on the Mockba Mod discord and [Amit Talwar](https://github.com/intelliriffer).

[Download Force Firmware](https://drive.google.com/drive/folders/1D_2jUc-XTDUkOrtrRkJPoW_uDM8R90ft?usp=sharing)

[Download MPC Firmware (Gen 1)](https://drive.google.com/drive/folders/1-a0xdDavSPyfv_s0dQvgRm9YVoCTKOri?usp=sharing)

The modifications and scripts are under various open source licences, but ***the original content in the firmware is © InMusic, Inc. and it's subsidiaries.***

# Ok I tried it, how do I get back
You can install any official firmware provided by AKAI and it will overwrite any changes made by these hacked firmwares.

# What is this
This is the same firmware you get from AKAI, except it has SSH and mouse enabled. Nothing else is modified, all functionality is still the same. You can use SSH to move and edit files in the internal file system and a mouse for more precise editing.

# Configuration
There is no configuration you need to do, stuff should work out-of-the-box and into the box. The mouse has a configuration file located in /etc/force_cursor.conf where you can set the speed of the cursor, but it shouldn't be necessary.

# What not to do
I take no responsibility or be held liable when your precious InMusic bleep bloop box explodes if you use these firmwares. It will and you are warned. InMusic Brands are also completely out of this as installing a modified 3rd party software into the machine effectively voids warranty.

Do not mess with the internals. You can move and delete stuff to your liking but when you do, you will probably brick your machine into a state where it can not be recovered anymore.

# You probably injected some spy software into the firmware
I'll provide all the tools here in this repository so you can do the modifications yourself if you want to be absolutely sure of what is going into the package. You can also decompress the firmware and do a diff against the official stock to see all the files and file entries.

The modifications are really minimal. Opening permissions for SSH, copying necessary networking files from the host, injecting a preload into the systemd service of the MPC OS so we can capture the DRM (Direct Rendering Manager) thread that hides the mouse cursor and adding the library that actually draws a cursor on screen.

