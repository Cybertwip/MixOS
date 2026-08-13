#!/bin/bash
# See finish.sh: the bye.gif this used to play went with the roms tree it lived in.
if [ ! -e "/home/virtua/.config/.SWAPPOWERANDSUSPEND" ]; then
  sudo systemctl suspend
else
  printf "\033c" >> /dev/tty1
  printf "\n\n\n\n\n\n\n      PEACE!" >> /dev/tty1
  sudo systemctl poweroff
fi
