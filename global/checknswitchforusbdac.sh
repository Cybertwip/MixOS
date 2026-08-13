#!/bin/bash

DAC="0"
DAC_EXIST=""
NUM_CHECK='^[1-9]+$'

if [[ "$(tr -d '\0' < /proc/device-tree/compatible)" == *"rk3326"* ]]; then
  DEVICE="rk3326"
else
  if [ "$(cat /home/virtua/.config/.DEVICE)" == "RG353V" ]; then
    DEVICE="rg353v"
  else
    DEVICE="rg503"
  fi
fi

# These next 2 gaurds are to address a potential issue in which .asoundrc and .asoundrcbak
# get potentially deleted or contentds deleted for some reason by the for loop.
if [ ! -e "/home/virtua/.asoundrcbak" ] || [ $(stat -c %s "/home/virtua/.asoundrcbak") = "0" ]; then
  sudo chown virtua:virtua /home/virtua/.asoundrcbak
  cp -f /usr/local/bin/.asoundbackup/.asoundrcbak.${DEVICE} /home/virtua/.asoundrcbak
  sudo chown virtua:virtua /home/virtua/.asoundrcbak
fi

if [ ! -e "/home/virtua/.asoundrc" ] || [ $(stat -c %s "/home/virtua/.asoundrc") = "0" ]; then
  sudo chown virtua:virtua /home/virtua/.asoundrc
  cp -f /home/virtua/.asoundrcbak /home/virtua/.asoundrc
  sudo chown virtua:virtua /home/virtua/.asoundrc
  if [[ "$(tr -d '\0' < /proc/device-tree/compatible)" == *"rk3566"* ]]; then
    if [ "$(cat /home/virtua/.config/.DEVICE)" == "RGB30" ] || [ "$(cat /home/virtua/.config/.DEVICE)" == "RK2023" ] ; then
      amixer -q sset 'Playback Path' HP
    else
      amixer -q sset 'Playback Path' SPK
    fi
  fi
fi

for (( ; ; ))
do
  if [ ! -e "/dev/snd/controlC7" ] && [[ "$DAC_EXIST" != "None" ]]; then
    sed -i '/hw:[0-9]/s//hw:0/' /home/virtua/.asoundrcbak /home/virtua/.asoundrc
    sudo systemctl restart ogage &
    DAC_EXIST="None"
    if [[ "$(tr -d '\0' < /proc/device-tree/compatible)" == *"rk3326"* ]]; then
      if [ -e "/etc/asound.conf" ]; then
        rm -f /etc/asound.conf
      fi
    fi
  elif [ -e "/dev/snd/controlC7" ] && [[ "$DAC" != "$DAC_EXIST" ]]; then
    DAC=$(ls -l /dev/snd/controlC7 | awk 'NR>=control {print $11;}' | cut -d 'C' -f2)
    if [[ $DAC =~ $NUM_CHECK ]]; then
      readarray -t USB_DAC < <(amixer -q -c ${DAC} scontents | grep 'Simple mixer control ' | cut -d "'" -f2)
      for i in "${USB_DAC[@]}"
      do
        amixer -q -c ${DAC} sset "${i}" 100%
      done
      sed -i '/hw:[0-9]/s//hw:'$DAC'/' /home/virtua/.asoundrcbak /home/virtua/.asoundrc
      DAC_EXIST="$DAC"
      if [[ "$(tr -d '\0' < /proc/device-tree/compatible)" == *"rk3326"* ]]; then
        echo "defaults.pcm.card ${DAC}" > /etc/asound.conf
        echo "defaults.ctl.card ${DAC}" >> /etc/asound.conf
        echo "ctl.!default { type hw card 0 }" >> /etc/asound.conf
      fi
      sudo systemctl restart ogage &
    fi
  fi
  if [[ "$(tr -d '\0' < /proc/device-tree/compatible)" == *"rk3326"* ]] && [[ "$DAC" == "$DAC_EXIST" ]]; then
      for i in "${USB_DAC[@]}"
      do
        amixer -q -c ${DAC} sset "${i}" $(sudo -u virtua '/usr/local/bin/current_volume')
      done
  fi
  sleep 1
done
