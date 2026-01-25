#!/bin/sh

appname=mouseCursor
appTitle=MouseCurosr
appDir=mouseCursor

################ NO NEED TO EDIT BELOW THIS LINE ###############

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

runDir="$mmPath/AddOns/"
installroot="$mmPath/AddOns/$appDir/"
runScript="$runDir/run_$appname.sh"
mode=$1

echo "
***********************************************************
*   $appTitle AddOn Manager for Mockba Mod      *
***********************************************************
"
if [ "$mode" == "UNINSTALL" ]; then
    rm -f "/dev/shm/.mouseCursor" 2>/dev/null
    rm -f "$runScript"
    echo "$appTitle has been disabled"
    echo "Restarting Force Application "
    
    respawn
echo "Using Disable Option, and Delete cursorMouse Folder fron Addons"
    
fi

if [ "$mode" == "DISABLE" ]; then
    rm -f "/dev/shm/.mouseCursor" 2>/dev/null
    rm -f "$runScript"
    echo "$appTitle has been disabled"
    echo "Restarting Force Application "
    echo "It Is Recommeded that you reboot force for proper disabling!"
    respawn
fi

if [ "$mode" == "ENABLE" ]; then
    bf=0
   # while [ $bf -ne 96 ] && [ $bf -ne 128 ] && [ $bf -ne 64 ]; do
    #    read -p "Please Enter Required Buffer Period Size from [64, 96, 128]: " bf
    #done

    #echo $bf >"$installroot/.customBufferSize"
    #echo $bf >"/dev/shm/.customBufferSize"

    cp -f "$installroot/run_$appname.sh" "$runScript"
    cp -f "$installroot/device.txt" "/dev/shm/.mouseCursor"
    echo "$appTitle has been enabled"
    echo "Restarting Force Application.."
    respawn

fi
