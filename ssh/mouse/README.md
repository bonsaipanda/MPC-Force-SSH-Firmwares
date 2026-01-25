# Mouse on the AKAI Force / MPC Live
This repository contains all the source code and files needed for the mouse. The original mod is for the Mockba Mod and this is a very much stripped down version of it, just providing a basic mouse cursor and nothing more.

You can build the library with your own modifications (if you want to change the cursor for example) and replace the .so in the ./usr/lib/ folder with your own version.

To test your build:
* SSH into your device and issue:`systemctl stop acvs.service` to stop MPC OS
* Replace the library in /usr/lib/
* Issue command `systemctl start acvs.service` to boot up MPC OS back up again

You can find the original source code package unmodified in the no3z folder.