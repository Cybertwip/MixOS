#!/bin/bash

if [ ! -z $(grep Off /home/virtua/.config/.TIMEOUT | tr -d '\0') ]; then
  sudo systemctl stop autosuspend &
  sudo systemctl disable autosuspend &
  echo "Disabled Auto Suspend daemon" | sudo tee /dev/kmsg
else
  sudo systemctl restart autosuspend &
  sudo systemctl enable autosuspend &
  echo "Enabled Auto Suspend daemon for $(cat /home/virtua/.config/.TIMEOUT) minutes" | sudo tee /dev/kmsg
fi
