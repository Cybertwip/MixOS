#!/bin/bash
# The ffplay of /roms/shutdownimages/bye.gif that used to sit between the clear and the
# message is gone with the roms tree: it was an EmulationStation shutdown animation, the
# only file the build ever put in that directory, and there is no front end here to have
# a house style.  What is left is the two lines that were always the useful part.
if [ ! -e "/home/virtua/.config/.SWAPPOWERANDSUSPEND" ]; then
  printf "\033c" >> /dev/tty1
  printf "\n\n\n\n\n\n\n      PEACE!" >> /dev/tty1
  sudo systemctl poweroff
else
  sudo systemctl suspend
fi
