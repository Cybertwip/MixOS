#!/bin/bash

# Bring wlan0 up as an access point: hostapd for the radio, dnsmasq for addresses, and a
# static 192.168.1.1 on the interface.  It was written for RetroArch's local netplay,
# which is not on this image any more; it is kept because an ad-hoc network from the
# handheld is also how you reach it with no router around -- ssh, samba and the OTA
# updater all work over it.
#
# Called as: ap_mode.sh Enable [suffix] | ap_mode.sh Disable

function Enable {
  nmcli d disconnect wlan0 > /dev/null
  host_ap_masked=$(systemctl status hostapd.service | grep masked)
  if [ ! -z $1 ]; then
    sudo sed -i "/ssid\=/c\ssid\=MixOS_AP_$1" /etc/hostapd/hostapd.conf
  else
    sudo sed -i "/ssid\=/c\ssid\=MixOS_AP" /etc/hostapd/hostapd.conf
  fi
  if [ ! -z "${host_ap_masked}" ]; then
    sudo systemctl unmask hostapd.service
    sudo systemctl disable hostapd.service
    sudo systemctl disable dnsmasq.service
  fi
  sudo systemctl restart hostapd.service
  if [ $? != 0 ]; then
    echo "Failed setting up hostapd"
    Disable Fail
    exit 1
  fi
  sudo systemctl restart dnsmasq.service
  if [ $? != 0 ]; then
    echo "Failed setting up dnsmasq"
    Disable Fail
    exit 1
  fi
  sudo ip addr add 192.168.1.1/24 dev wlan0
  if [ $? != 0 ]; then
    echo "Failed setting a static ip for wlan0"
    Disable Fail
    exit 1
  fi
  echo "Success!"
}

function Disable {
  sudo systemctl stop hostapd.service
  sudo systemctl stop dnsmasq.service
  sudo ip addr del 192.168.1.1/24 dev wlan0
  #nmcli c delete "$(iw dev wlan0 info | grep ssid | cut -c 7-30)"
  nmcli d disconnect wlan0 > /dev/null
  nmcli d connect wlan0 > /dev/null &
  if [ "$1" = "Fail" ]; then
    echo "Fail!"
  else
    echo "Success!"
  fi
}

cmd=${1}
shift
if [[ -z ${cmd} ]]; then
  printf "\nNo valid argument provided such as Enable or Disable\n\n"
  exit 1
fi
$cmd $1

exit 0

