#!/bin/sh

# Set up the environment
mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

LD_LIB=/media/662522/AddOns/mouseCursor/libforce_cursor.so

SETLIBS() {

    if [ -f "$mmLD_PRELOAD_VAR" ]; then
        #if exists check if its already loaded, otherwise append to end.

        FC=$(cat "$mmLD_PRELOAD_VAR")

        if [[ "$FC" != *"$LD_LIB"* ]]; then
            #always prepend to start
            echo "$LD_LIB $FC " >"$mmLD_PRELOAD_VAR"
        fi

    else

        echo "$LD_LIB" >"$mmLD_PRELOAD_VAR"
    fi

}

if test "$1" != "kill"; then
    SETLIBS
    cp -f /media/662522/AddOns/mouseCursor/device.txt "/dev/shm/.mouseCursor"
    sleep 1
fi
