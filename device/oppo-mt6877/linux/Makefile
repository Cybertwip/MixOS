# OPPO MT6877 out-of-tree modules. Built in the VM against mainline 6.12 LTS,
# one module per subsystem, each staged behind its own oppo.* command-line
# word (see build-in-vm.sh). Matches the shape of device/j36-ultra/linux/.

obj-m += oppo_mt6877_consys.o
obj-m += oppo_mt6877_wifi.o
oppo_mt6877_wifi-y := oppo_mt6877_wifi_main.o \
		      oppo_mt6877_wifi_wmt.o \
		      oppo_mt6877_wifi_net.o

obj-m += oppo_mt6877_panel_td4330.o
obj-m += oppo_mt6877_touch_nt36672.o
obj-m += oppo_mt6877_audio.o
obj-m += oppo_mt6877_modem.o
obj-m += oppo_mt6877_input.o
obj-m += oppo_mt6877_pmic.o
