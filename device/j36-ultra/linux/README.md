# J36 Ultra Linux bring-up adapter

`j36_mt6592_input.c` is an out-of-tree ARM Linux platform driver for the
`j36,j36-ultra-input` DT node. It is polled, and it is confined to the input
path:

- reads GPIO DIN, with a pull-up armed so "active low" has a defined idle,
- muxes the three keypad pads the boot chain leaves parked, and only those,
- ungates the keypad's 32 kHz clock in the MT6323 PMIC over PWRAP,
- reads/enables MT6592 KPD scan memory,
- runs the MVII stop/settle/start AUXADC conversion sequence,
- reports one Linux gamepad matching vendor/product `2454:6500`,
- never writes MMSYS, DSI, MIPI-TX, panel, or backlight registers.

It used to say "reads GPIO DIN without changing pinmux", and that was a
description of a keypad with seven dead buttons. Two things on this board are not
optional, and neither is visible from a register dump that looks healthy:

- **Three of the keypad block's eight pads arrive parked.** The preloader muxes
  KPROW0/1 and KPCOL0/1/2; KPROW3 (pad 11), KPCOL3 (pad 12) and KPCOL4 (pad 2)
  arrive as plain GPIO, so the block scans two rows against three columns and
  only matrix bits `{0,1,2,9,10,11}` ever change. VOL-, VOL+, SELECT, START,
  MENU, R2 and A read as never pressed under a perfectly correct keymap. The
  needed mode differs per pad (1, 3 and 6) and each was measured with `kpdmode
  <pad>` in the MVII LK console; the pads and modes live in the DT keypad node as
  `j36,kpd-strobe-pads` / `j36,kpd-sense-pads`.
- **The keypad's 32 kHz clock is in the PMIC, not the SoC.** With MT6323 register
  0x40 bit 0 still set, the scan engine sits with `KP_EN` set, `KP_DEBOUNCE`
  loaded and every scan memory reading its idle all-ones pattern -- which is
  indistinguishable from a correctly configured matrix that nobody is touching.

Pad 93 is the third trap. It is KPROW2's pad, which this board spent on D-pad
UP's EINT (hence dead matrix row 2), and it will not idle high on the internal
pull-up: it reads 0 held or released, because something loads it harder than that
resistor can fight. Such a pad is driven high for about a microsecond per poll and
sampled while driven. Which pads need that is measured at probe, not hardcoded,
so a board revision that populates the missing pull-up never takes the path.

The visible boot path remains stock-LK scanout through `simple-framebuffer`.
