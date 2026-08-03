# J36 Ultra Linux bring-up adapter

`j36_mt6592_input.c` is an out-of-tree ARM Linux platform driver for the
`j36,j36-ultra-input` DT node. It is deliberately polled and non-destructive:

- reads GPIO DIN without changing pinmux,
- reads/enables MT6592 KPD scan memory,
- runs the MVII stop/settle/start AUXADC conversion sequence,
- reports one Linux gamepad matching vendor/product `2454:6500`,
- never writes MMSYS, DSI, MIPI-TX, panel, or backlight registers.

The visible boot path remains stock-LK scanout through `simple-framebuffer`.
