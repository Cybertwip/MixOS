#!/bin/bash

function remove_mixos_devenv() {
  for m in proc dev/pts dev dev sys
  do
    if grep -qs "MixOS_devenv${bit}/${m} " /proc/mounts; then
      sudo umount -l MixOS_devenv${bit}/${m}
      verify_action
      sync
      sleep 1
    fi
  done
  (cat /proc/mounts | grep -qs "MixOS_devenv${bit}") && sudo umount -l MixOS_devenv${bit}
  return 0
}

if [ "$1" == "32" ]; then
  bit="32"
else
  bit=""
fi

remove_mixos_devenv
