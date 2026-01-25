Instructions for mouseCursor Addon
module Credits: @no3z (discord)
------------------------------------------------------------
This AddOn allows you to use a mouse on the force.

The device.txt needs 2 lines . Line 1 is your mouse input device event 
and second line is cursor speed Multiplier values between 0.1 and 5.0

Its possible that your Force might have different devices connected and 
the actual input device might be different than /dev/input/event2
in that case you will have to find your device id and change event2 to its respective
event number say event4 or whatever.
you can get a list of the input devices using the following command 
and you can use the event value from H: parameter
command : cat /proc/bus/input/devices