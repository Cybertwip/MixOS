#!/bin/bash
# Route playback to speaker + headphone on boards whose codec has one knob for it.
#
# 'Playback Path' is an rk817 control.  The J36 Ultra's codec is driven by
# j36-mt6592-audio, which has no such element, so this printed
#
#     amixer: Cannot find the given element from control sysdefault:0
#
# on every J36 boot -- to the console, which during 351mp.service is the panel
# with the splash on it.  Ask amixer whether the control is there before setting
# it: a board that routes its outputs in the driver has nothing to say here.
if amixer -c 0 cget iface=MIXER,name='Playback Path' >/dev/null 2>&1; then
    amixer -c 0 cset iface=MIXER,name='Playback Path' SPK_HP >/dev/null 2>&1
fi

exit 0
